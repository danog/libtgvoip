//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "controller/net/JitterBuffer.h"
#include "VoIPController.h"
#include "VoIPServerConfig.h"
#include "tools/logging.h"
#include <math.h>

using namespace tgvoip;

JitterArray::JitterArray(bool isEC) : isEC(isEC){};
bool JitterArray::has(uint32_t seq)
{
    return get(seq) != slots.end();
}
uint32_t JitterArray::put(uint32_t seq, std::unique_ptr<Buffer> &&buffer)
{
    if (empty())
    {
        LOGW("First packet after empty, seq=backSeq=frontSeq=%u, isEC=%u", seq, isEC);
        backSeq = frontSeq = seq;
        back = front = 0;
        slots[front] = std::move(buffer);
        return 0;
    }
    if (seq < backSeq)
    {
        LOGE("Cannot insert packet with seq=%u, too old compared to oldestSeq=%u", seq, backSeq);
        return 0;
    }
    uint32_t advance = 0;
    uint32_t diff = seq - backSeq;
    if (diff > JITTER_SLOT_COUNT)
    {
        advance = diff - JITTER_SLOT_COUNT;
        LOGW("Not enough slots, replacing %u slots", advance);
        this->advance(backSeq + advance);
    }
    LOGW("Inserting packet with seq=%u, diff=%u, isEC=%u, frontSeq=%u, backSeq=%u", seq, diff, isEC, frontSeq, backSeq);
    front += diff;
    front %= JITTER_SLOT_COUNT;
    frontSeq = seq;
    slots[front] = std::move(buffer);
    return advance;
}

std::array<std::unique_ptr<Buffer>, JITTER_SLOT_COUNT>::iterator JitterArray::get(uint32_t seq)
{
    if (empty() || seq > frontSeq || seq < backSeq)
        return slots.end();
    auto res = std::next(slots.begin(), getOffset(seq));
    if (!res->get())
    {
        return slots.end();
    }
    return res;
}

void JitterArray::advance(uint32_t seq)
{
    if (empty())
        return;
    if (seq > frontSeq)
    {
        LOGW("Advancing, seq=%u > frontSeq=%u, clearing all", seq, frontSeq);
        clear();
        return;
    }
    if (seq < backSeq) 
    {
        LOGW("Can't clear, seq=%u < backSeq=%u", seq, backSeq);
        return;
    }
    LOGW("Clearing until seq=%u, backSeq=%u, frontSeq=%u", seq, backSeq, frontSeq);
    auto until = getOffset(seq);
    for (uint8_t offset = back; offset != until; offset = (offset + 1) % JITTER_SLOT_COUNT)
    {
        LOGW("Clearing offset %hhu", offset);
        if (slots[offset])
            slots[offset] = nullptr;
    }
    back = until;
    backSeq = seq;
}

void JitterArray::clear()
{
    frontSeq = backSeq = INVALID_SEQ;
    front = back = 0;
    std::for_each(slots.begin(), slots.end(), [](std::unique_ptr<Buffer> &t) {
        t = nullptr;
    });
}

bool JitterArray::empty()
{
    return frontSeq == INVALID_SEQ;
}

uint8_t JitterArray::count()
{
    if (empty())
        return 0;
    if (back == front && slots[back])
        return 1;
    uint8_t count = 0;
    for (uint8_t offset = back; offset != front; offset = (offset + 1) % JITTER_SLOT_COUNT)
    {
        if (slots[offset])
            count++;
    }
    return count;
}

JitterBuffer::JitterBuffer(uint32_t step) : step(step)
{
    if (step < 30)
    {
        minMinDelay = ServerConfig::GetSharedInstance()->GetUInt("jitter_min_delay_20", 6);
        maxMinDelay = ServerConfig::GetSharedInstance()->GetUInt("jitter_max_delay_20", 25);
        maxUsedSlots = ServerConfig::GetSharedInstance()->GetUInt("jitter_max_slots_20", 50);
    }
    else if (step < 50)
    {
        minMinDelay = ServerConfig::GetSharedInstance()->GetUInt("jitter_min_delay_40", 4);
        maxMinDelay = ServerConfig::GetSharedInstance()->GetUInt("jitter_max_delay_40", 15);
        maxUsedSlots = ServerConfig::GetSharedInstance()->GetUInt("jitter_max_slots_40", 30);
    }
    else
    {
        minMinDelay = ServerConfig::GetSharedInstance()->GetUInt("jitter_min_delay_60", 2);
        maxMinDelay = ServerConfig::GetSharedInstance()->GetUInt("jitter_max_delay_60", 10);
        maxUsedSlots = ServerConfig::GetSharedInstance()->GetUInt("jitter_max_slots_60", 20);
    }
    lossesToReset = ServerConfig::GetSharedInstance()->GetUInt("jitter_losses_to_reset", 20);
    resyncThreshold = ServerConfig::GetSharedInstance()->GetDouble("jitter_resync_threshold", 1.0);
#ifdef TGVOIP_DUMP_JITTER_STATS
#ifdef TGVOIP_JITTER_DUMP_FILE
    dump = fopen(TGVOIP_JITTER_DUMP_FILE, "w");
#elif defined(__ANDROID__)
    dump = fopen("/sdcard/tgvoip_jitter_dump.txt", "w");
#else
    dump = fopen("tgvoip_jitter_dump.txt", "w");
#endif
    tgvoip_log_file_write_header(dump);
    fprintf(dump, "PTS\tRTS\tNumInBuf\tAJitter\tADelay\tTDelay\n");
#endif
    Reset();
}

JitterBuffer::~JitterBuffer()
{
    Reset();
}

void JitterBuffer::SetMinPacketCount(uint32_t count)
{
    LOGI("jitter: set min packet count %u", count);
    minDelay = count;
    minMinDelay = count;
    //Reset();
}

int JitterBuffer::GetMinPacketCount()
{
    return (int)minDelay;
}
double JitterBuffer::GetTimeoutWindow()
{
    return (lossesToReset * step) / 1000.0;
}

void JitterBuffer::HandleInput(std::unique_ptr<Buffer> &&buf, uint32_t timestamp, bool isEC)
{
    MutexGuard m(mutex);

    auto &slots = isEC ? slotsEc : slotsMain;
    if (slots.has(timestamp))
    {
        return;
    }

    gotSinceReset++;
    if (wasReset)
    {
        wasReset = false;
        outstandingDelayChange = 0;
        nextFetchTimestamp = timestamp - minDelay;
        first = true;
        LOGI("jitter: resyncing, next timestamp = %lld (step=%d, minDelay=%f)", (long long int)nextFetchTimestamp, step, (double)minDelay);
    }

    // Clear packets older than the last packet pulled from jitter buffer
    slotsMain.advance(nextFetchTimestamp - 1);
    slotsEc.advance(nextFetchTimestamp - 1);

    // Time deviation check
    double time = VoIPController::GetCurrentTime();
    if (expectNextAtTimeMs)
    {
        deviationHistory.Add(expectNextAtTimeMs - time);
        expectNextAtTimeMs += step / 1000.0;
    }
    else
    {
        expectNextAtTimeMs = time + step / 1000.0;
    }

    // Late packet check
    if (timestamp < nextFetchTimestamp)
    {
        if (!isEC) // If EC, do not count as late packet
        {
            LOGW("jitter: would drop packet with timestamp %d because it is late but not hopelessly (nextSeq=%d)", timestamp, nextFetchTimestamp);
            latePacketCount++;
            lostPackets--;
        }
    }
    else if (timestamp < nextFetchTimestamp - 1)
    {
        if (!isEC) // If EC, do not count as late packet
        {
            LOGW("jitter: dropping packet with timestamp %d because it is too late (nextSeq=%d)", timestamp, nextFetchTimestamp);
            latePacketCount++;
        }
        return;
    }

    if (timestamp > lastPutTimestamp)
        lastPutTimestamp = timestamp;

    unsigned int delay = GetCurrentDelay();
    if (delay > 5)
    {
        LOGW("jitter: delay too big upon put (%u), dropping packets", delay);
        nextFetchTimestamp += 1;
        slotsEc.advance(nextFetchTimestamp);
        slotsMain.advance(nextFetchTimestamp);
    }
    nextFetchTimestamp += slots.put(timestamp, std::move(buf));

#ifdef TGVOIP_DUMP_JITTER_STATS
    fprintf(dump, "%u\t%.03f\t%d\t%.03f\t%.03f\t%.03f\n", timestamp, time, GetCurrentDelay(), lastMeasuredJitter, lastMeasuredDelay, minDelay);
#endif
    prevRecvTime = time;
}

void JitterBuffer::Reset()
{
    wasReset = true;
    needBuffering = true;
    lastPutTimestamp = 0;

    slotsEc.clear();
    slotsMain.clear();

    delayHistory.Reset();
    lateHistory.Reset();
    adjustingDelay = false;
    lostSinceReset = 0;
    gotSinceReset = 0;
    expectNextAtTimeMs = 0;
    deviationHistory.Reset();
    outstandingDelayChange = 0;
    dontChangeDelayFor = 0;
}

std::unique_ptr<Buffer> JitterBuffer::HandleOutput(bool advance, int &playbackScaledDuration, bool &isEC)
{
    MutexGuard m(mutex);

    if (first)
    {
        first = false;

        unsigned int delay = GetCurrentDelay();
        if (delay > 5)
        {
            LOGW("jitter: delay too big upon start (%u), dropping packets", delay);
            nextFetchTimestamp += delay - GetMinPacketCount();
            slotsEc.advance(nextFetchTimestamp);
            slotsMain.advance(nextFetchTimestamp);
        }
    }

    jitter_packet_t pkt;
    int result = GetInternal(pkt, advance);
    if (outstandingDelayChange)
    {
        if (outstandingDelayChange < 0)
        {
            playbackScaledDuration = 40;
            outstandingDelayChange += 20;
        }
        else
        {
            playbackScaledDuration = 80;
            outstandingDelayChange -= 20;
        }
        //LOGV("outstanding delay change: %d", outstandingDelayChange);
    }
    else if (advance && GetCurrentDelay() == 0)
    {
        //LOGV("stretching packet because the next one is late");
        playbackScaledDuration = 80;
    }
    else
    {
        playbackScaledDuration = 60;
    }
    
    if (result == JR_OK)
    {
        isEC = pkt.isEC;
        return std::move(pkt.buffer);
    }
    else
    {
        return nullptr;
    }
}

int JitterBuffer::GetInternal(jitter_packet_t &pkt, bool advance)
{
    bool hasMain = slotsMain.has(nextFetchTimestamp);
    bool hasEc = slotsEc.has(nextFetchTimestamp);
    if (hasMain || hasEc)
    {
        auto slot = hasMain ? slotsMain.get(nextFetchTimestamp) : slotsEc.get(nextFetchTimestamp);

        pkt.timestamp = nextFetchTimestamp;
        pkt.buffer = std::move(*slot);
        pkt.isEC = !hasMain;

        nextFetchTimestamp++;

        lostCount = 0;
        needBuffering = false;
        return JR_OK;
    }

    LOGV("jitter: found no packet for timestamp %lld (last put = %d, lost = %d)", (long long int)nextFetchTimestamp, lastPutTimestamp, lostCount);

    if (advance)
        nextFetchTimestamp++;

    if (!needBuffering)
    {
        lostCount++;
        lostPackets++;
        lostSinceReset++;
        if (lostCount >= lossesToReset || (gotSinceReset > minDelay * 25 && lostSinceReset > gotSinceReset / 2))
        {
            LOGW("jitter: lost %d packets in a row, resetting", lostCount);
            //minDelay++;
            dontIncMinDelayFor = 16;
            dontDecMinDelayFor += 128;
            auto currentDelay = GetCurrentDelay();
            LOGW("currentDelay=%u, minDelay=%lf, nextFetchSeq=%u", currentDelay, minDelay.load(), nextFetchTimestamp)
            if (currentDelay < minDelay)
                nextFetchTimestamp -= minDelay - currentDelay;
            LOGW("currentDelay=%u, minDelay=%lf, nextFetchSeq=%u", currentDelay, minDelay.load(), nextFetchTimestamp)
            lostCount = 0;
            Reset();
        }

        return JR_MISSING;
    }
    return JR_BUFFERING;
}

unsigned int JitterBuffer::GetCurrentDelay()
{
    return std::max(slotsEc.count(), slotsMain.count());
}

void JitterBuffer::Tick()
{
    MutexGuard m(mutex);
    int i;

    lateHistory.Add(latePacketCount);
    latePacketCount = 0;
    bool absolutelyNoLatePackets = lateHistory.Max() == 0;

    double avgLate16 = lateHistory.Average(16);
    //LOGV("jitter: avg late=%.1f, %.1f, %.1f", avgLate16, avgLate32, avgLate64);
    if (avgLate16 >= resyncThreshold)
    {
        LOGV("resyncing: avgLate16=%f, resyncThreshold=%f", avgLate16, resyncThreshold);
        wasReset = true;
    }

    if (absolutelyNoLatePackets)
    {
        if (dontDecMinDelayFor > 0)
            dontDecMinDelayFor--;
    }

    delayHistory.Add(GetCurrentDelay());
    avgDelay = delayHistory.Average(32);

    double stddev = 0;
    double avgdev = deviationHistory.Average();
    for (i = 0; i < 64; i++)
    {
        double d = (deviationHistory[i] - avgdev);
        stddev += (d * d);
    }
    stddev = sqrt(stddev / 64);
    uint32_t stddevDelay = std::clamp(static_cast<uint32_t>(ceil(stddev * 2 * 1000 / step)), minMinDelay, maxMinDelay);
    //LOGW("Average delay diff of %lf s, stddev=%lf s, stddevPacket=%u (minDelayPacket=%lf)", avgDelay, stddev, stddevDelay, minDelay.load());

    // The difference between estimated time of arrival and actual TOA (=packet jitter) is used to calculate standard deviation of packet jitter.
    // if the packet jitter is normally and consistently bigger than the jitter buffer delay, increase the jitter buffer delay.
    if (stddevDelay != minDelay)
    {
        int32_t diff = std::clamp((int32_t)(stddevDelay - minDelay), -1, 1);
        if (diff > 0)
        {
            dontDecMinDelayFor = 100;
        }

        if ((diff > 0 && dontIncMinDelayFor == 0) || (diff < 0 && dontDecMinDelayFor == 0))
        {
            //nextFetchTimestamp+=diff*(int32_t)step;
            minDelay.store(minDelay + diff);
            outstandingDelayChange += diff * 60;
            dontChangeDelayFor += 32;
            //LOGD("new delay from stddev %f", minDelay);
            if (diff < 0)
            {
                dontDecMinDelayFor += 25;
            }
            if (diff > 0)
            {
                dontIncMinDelayFor = 25;
            }
        }
    }
    lastMeasuredJitter = stddev;
    lastMeasuredDelay = stddevDelay;
    //LOGV("stddev=%.3f, avg=%.3f, ndelay=%d, dontDec=%u", stddev, avgdev, stddevDelay, dontDecMinDelayFor);
    if (dontChangeDelayFor)
    {
        --dontChangeDelayFor;
    }
    else
    {
        //LOGW("avgDelay=%lf, minDelay=%lf", avgDelay, minDelay.load());
        if (avgDelay > minDelay + 0.5)
        {
            outstandingDelayChange -= avgDelay > minDelay + 2 ? 60 : 20;
            dontChangeDelayFor += 10;
        }
        else if (avgDelay < minDelay - 0.3)
        {
            outstandingDelayChange += 20;
            dontChangeDelayFor += 10;
        }
    }

    //LOGV("jitter: avg delay=%d, delay=%d, late16=%.1f, dontDecMinDelay=%d", avgDelay, delayHistory[0], avgLate16, dontDecMinDelay);
    /*if(!adjustingDelay) {
		if (((minDelay==1 ? (avgDelay>=3) : (avgDelay>=minDelay/2)) && delayHistory[0]>minDelay && avgLate16<=0.1 && absolutelyNoLatePackets && dontDecMinDelay<32 && min>minDelay)) {
			LOGI("jitter: need adjust");
			adjustingDelay=true;
		}
	}else{
		if(!absolutelyNoLatePackets){
			LOGI("jitter: done adjusting because we're losing packets");
			adjustingDelay=false;
		}else if(tickCount%5==0){
			LOGD("jitter: removing a packet to reduce delay");
			GetInternal(NULL, 0);
			expectNextAtTime=0;
			if(GetCurrentDelay()<=minDelay || min<=minDelay){
				adjustingDelay = false;
				LOGI("jitter: done adjusting");
			}
		}
	}*/

    tickCount++;
}

void JitterBuffer::GetAverageLateCount(double *out)
{
    out[0] = lateHistory.Average(16);
    out[1] = lateHistory.Average(32);
    out[2] = lateHistory.Average();
}

int JitterBuffer::GetAndResetLostPacketCount()
{
    MutexGuard m(mutex);
    int r = lostPackets;
    lostPackets = 0;
    return r;
}

double JitterBuffer::GetLastMeasuredJitter()
{
    return lastMeasuredJitter;
}

double JitterBuffer::GetLastMeasuredDelay()
{
    return lastMeasuredDelay;
}

double JitterBuffer::GetAverageDelay()
{
    return avgDelay;
}
