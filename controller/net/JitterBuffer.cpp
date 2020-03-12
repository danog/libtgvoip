//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "VoIPController.h"
#include "controller/net/JitterBuffer.h"
#include "tools/logging.h"
#include "VoIPServerConfig.h"
#include <math.h>

using namespace tgvoip;

JitterBuffer::JitterBuffer(uint32_t step) : step(step),
											slots{}
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

void JitterBuffer::HandleInput(unsigned char *data, size_t len, uint32_t timestamp, bool isEC)
{
	MutexGuard m(mutex);
	jitter_packet_t pkt;
	pkt.size = len;
	pkt.buffer = Buffer::Wrap(data, len, [](void *) {}, [](void *a, size_t) -> void * { return a; });
	pkt.timestamp = timestamp;
	pkt.isEC = isEC;
	PutInternal(pkt, !isEC);
	//LOGV("in, ts=%d, ec=%d", timestamp, isEC);
}

void JitterBuffer::PutInternal(jitter_packet_t &pkt, bool overwriteExisting)
{
	if (pkt.size > JITTER_SLOT_SIZE)
	{
		LOGE("The packet is too big to fit into the jitter buffer");
		return;
	}

	auto existing = std::find_if(slots.begin(), slots.end(), [timestamp = pkt.timestamp](const jitter_packet_t &slot) -> bool {
		return slot.timestamp == timestamp && !slot.buffer.IsEmpty();
	});
	if (existing != slots.end())
	{
		if (overwriteExisting)
		{
			existing->buffer.CopyFromOtherBuffer(pkt.buffer, pkt.size);
			existing->size = pkt.size;
			existing->isEC = pkt.isEC;
		}
		return;
	}

	gotSinceReset++;
	if (wasReset)
	{
		wasReset = false;
		outstandingDelayChange = 0;
		nextFetchTimestamp = static_cast<int64_t>(static_cast<int64_t>(pkt.timestamp) - step * minDelay);
		first = true;
		LOGI("jitter: resyncing, next timestamp = %lld (step=%d, minDelay=%f)", (long long int)nextFetchTimestamp, step, (double)minDelay);
	}

	for (auto &slot : slots)
	{
		// Clear packets older than the last packet pulled from jitter buffer
		if (slot.timestamp < nextFetchTimestamp - 1 && !slot.buffer.IsEmpty())
		{
			slot.buffer = Buffer();
		}
	}

	/*double prevTime=0;
	uint32_t closestTime=0;
	for(i=0;i<JITTER_SLOT_COUNT;i++){
		if(slots[i].buffer!=NULL && pkt.timestamp-slots[i].timestamp<pkt.timestamp-closestTime){
			closestTime=slots[i].timestamp;
			prevTime=slots[i].recvTime;
		}
	}*/

	// Time deviation check
	double time = VoIPController::GetCurrentTime();
	if (expectNextAtTime)
	{
		deviationHistory.Add(expectNextAtTime - time);
		expectNextAtTime += step / 1000.0;
	}
	else
	{
		expectNextAtTime = time + step / 1000.0;
	}

	// Late packet check
	if (pkt.timestamp < nextFetchTimestamp)
	{
		//LOGW("jitter: would drop packet with timestamp %d because it is late but not hopelessly", pkt.timestamp);
		latePacketCount++;
		lostPackets--;
	}
	else if (pkt.timestamp < nextFetchTimestamp - 1)
	{
		//LOGW("jitter: dropping packet with timestamp %d because it is too late", pkt.timestamp);
		latePacketCount++;
		return;
	}

	if (pkt.timestamp > lastPutTimestamp)
		lastPutTimestamp = pkt.timestamp;

	// If no free slots or too many used up slots to be useful
	auto slot = GetCurrentDelay() >= maxUsedSlots ? slots.end() : std::find_if(slots.begin(), slots.end(), [](const jitter_packet_t &a) -> bool {
		return a.buffer.IsEmpty();
	});

	if (slot == slots.end())
	{
		slot = std::min_element(slots.begin(), slots.end(), [](const jitter_packet_t &a, const jitter_packet_t &b) -> bool {
			return !a.buffer.IsEmpty() && a.timestamp < b.timestamp;
		});
		slot->buffer = Buffer();
		Advance();
	}

	slot->timestamp = pkt.timestamp;
	slot->size = pkt.size;
	slot->buffer = bufferPool.Get();
	slot->recvTimeDiff = time - prevRecvTime;
	slot->isEC = pkt.isEC;
	slot->buffer.CopyFromOtherBuffer(pkt.buffer, pkt.size);
#ifdef TGVOIP_DUMP_JITTER_STATS
	fprintf(dump, "%u\t%.03f\t%d\t%.03f\t%.03f\t%.03f\n", pkt.timestamp, time, GetCurrentDelay(), lastMeasuredJitter, lastMeasuredDelay, minDelay);
#endif
	prevRecvTime = time;
}

void JitterBuffer::Reset()
{
	wasReset = true;
	needBuffering = true;
	lastPutTimestamp = 0;
	std::for_each(slots.begin(), slots.end(), [](jitter_packet_t &t) {
		t.buffer = Buffer();
	});
	delayHistory.Reset();
	lateHistory.Reset();
	adjustingDelay = false;
	lostSinceReset = 0;
	gotSinceReset = 0;
	expectNextAtTime = 0;
	deviationHistory.Reset();
	outstandingDelayChange = 0;
	dontChangeDelay = 0;
}

size_t JitterBuffer::HandleOutput(unsigned char *buffer, size_t len, int offsetInSteps, bool advance, int &playbackScaledDuration, bool &isEC)
{
	jitter_packet_t pkt;
	pkt.buffer = Buffer::Wrap(buffer, len, [](void *) {}, [](void *a, size_t) -> void * { return a; });
	pkt.size = len;

	MutexGuard m(mutex);

	if (first)
	{
		first = false;

		unsigned int delay = GetCurrentDelay();
		if (delay > 5)
		{
			LOGW("jitter: delay too big upon start (%u), dropping packets", delay);
			for (; delay > GetMinPacketCount(); --delay)
			{
				auto slot = std::find_if(slots.begin(), slots.end(), [&](const jitter_packet_t &a) -> bool {
					return a.timestamp == nextFetchTimestamp;
				});
				if (slot != slots.end() && !slot->buffer.IsEmpty())
				{
					slot->buffer = Buffer();
				}
				Advance();
			}
		}
	}

	int result = GetInternal(pkt, offsetInSteps, advance);
	if (outstandingDelayChange != 0)
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
		return pkt.size;
	}
	else
	{
		return 0;
	}
}

int JitterBuffer::GetInternal(jitter_packet_t &pkt, int offset, bool advance)
{
	/*if(needBuffering && lastPutTimestamp<nextFetchTimestamp){
		LOGV("jitter: don't have timestamp %lld, buffering", (long long int)nextFetchTimestamp);
		Advance();
		return JR_BUFFERING;
	}*/

	//needBuffering=false;

	int64_t timestampToGet = nextFetchTimestamp + offset * (int32_t)step;

	auto slot = std::find_if(slots.begin(), slots.end(), [timestampToGet](const jitter_packet_t &a) -> bool {
		return a.timestamp == timestampToGet && !a.buffer.IsEmpty();
	});

	if (slot != slots.end())
	{
		if (pkt.size < slot->size)
		{
			LOGE("jitter: packet won't fit into provided buffer of %d (need %d)", int(slot->size), int(pkt.size));
		}
		else
		{
			pkt.size = slot->size;
			pkt.timestamp = slot->timestamp;
			pkt.buffer.CopyFromOtherBuffer(slot->buffer, slot->size);
			pkt.isEC = slot->isEC;
		}
		slot->buffer = Buffer();
		if (offset == 0)
			Advance();
		lostCount = 0;
		needBuffering = false;
		return JR_OK;
	}

	LOGV("jitter: found no packet for timestamp %lld (last put = %d, lost = %d)", (long long int)timestampToGet, lastPutTimestamp, lostCount);

	if (advance)
		Advance();

	if (!needBuffering)
	{
		lostCount++;
		if (offset == 0)
		{
			lostPackets++;
			lostSinceReset++;
		}
		if (lostCount >= lossesToReset || (gotSinceReset > minDelay * 25 && lostSinceReset > gotSinceReset / 2))
		{
			LOGW("jitter: lost %d packets in a row, resetting", lostCount);
			//minDelay++;
			dontIncMinDelay = 16;
			dontDecMinDelay += 128;
			if (GetCurrentDelay() < minDelay)
				nextFetchTimestamp -= (int64_t)(minDelay - GetCurrentDelay());
			lostCount = 0;
			Reset();
		}

		return JR_MISSING;
	}
	return JR_BUFFERING;
}

void JitterBuffer::Advance()
{
	nextFetchTimestamp += step;
}

unsigned int JitterBuffer::GetCurrentDelay()
{
	return std::count_if(slots.begin(), slots.end(), [](const jitter_packet_t &a) -> bool {
		return !a.buffer.IsEmpty();
	});
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
		if (dontDecMinDelay > 0)
			dontDecMinDelay--;
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
	uint32_t stddevDelay = std::clamp((uint32_t)ceil(stddev * 2 * 1000 / step), minMinDelay, maxMinDelay);

	if (stddevDelay != minDelay)
	{
		int32_t diff = std::clamp((int32_t)(stddevDelay - minDelay), -1, 1);
		if (diff > 0)
		{
			dontDecMinDelay = 100;
		}

		if ((diff > 0 && dontIncMinDelay == 0) || (diff < 0 && dontDecMinDelay == 0))
		{
			//nextFetchTimestamp+=diff*(int32_t)step;
			minDelay.store(minDelay + diff);
			outstandingDelayChange += diff * 60;
			dontChangeDelay += 32;
			//LOGD("new delay from stddev %f", minDelay);
			if (diff < 0)
			{
				dontDecMinDelay += 25;
			}
			if (diff > 0)
			{
				dontIncMinDelay = 25;
			}
		}
	}
	lastMeasuredJitter = stddev;
	lastMeasuredDelay = stddevDelay;
	//LOGV("stddev=%.3f, avg=%.3f, ndelay=%d, dontDec=%u", stddev, avgdev, stddevDelay, dontDecMinDelay);
	if (dontChangeDelay)
	{
		--dontChangeDelay;
	}
	else
	{
		if (avgDelay > minDelay + 0.5)
		{
			outstandingDelayChange -= avgDelay > minDelay + 2 ? 60 : 20;
			dontChangeDelay += 10;
		}
		else if (avgDelay < minDelay - 0.3)
		{
			outstandingDelayChange += 20;
			dontChangeDelay += 10;
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