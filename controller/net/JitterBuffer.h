//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_JITTERBUFFER_H
#define LIBTGVOIP_JITTERBUFFER_H

#include "controller/media/MediaStreamItf.h"
#include "tools/BlockingQueue.h"
#include "tools/Buffers.h"
#include "tools/logging.h"
#include "tools/threading.h"
#include <algorithm>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#define JITTER_SLOT_COUNT 64
#define JR_OK 1
#define JR_MISSING 2
#define JR_BUFFERING 3

#define INVALID_SEQ 0xFFFFFFFF

namespace tgvoip
{

struct JitterArray
{
    JitterArray(bool isEC);

private:
    inline uint8_t getOffset(uint32_t seq)
    {
        return (back + (seq - backSeq)) % JITTER_SLOT_COUNT;
    }

public:
    bool has(uint32_t seq);
    uint32_t put(uint32_t seq, std::unique_ptr<Buffer> &&buffer);
    std::array<std::unique_ptr<Buffer>, JITTER_SLOT_COUNT>::iterator get(uint32_t seq);
    void advance(uint32_t seq);
    void clear();
    uint8_t count();
    bool empty();
    auto end()
    {
        return slots.end();
    }

    uint8_t front = 0;
    uint8_t back = 0;

    uint32_t frontSeq = INVALID_SEQ;
    uint32_t backSeq = INVALID_SEQ;

    bool isEC = false;
    std::array<std::unique_ptr<Buffer>, JITTER_SLOT_COUNT> slots;
};
class JitterBuffer
{
public:
    JitterBuffer(uint32_t step);
    ~JitterBuffer();
    void SetMinPacketCount(uint32_t count);
    int GetMinPacketCount();
    unsigned int GetCurrentDelay();
    double GetAverageDelay();
    void Reset();
    void HandleInput(std::unique_ptr<Buffer> &&buf, uint32_t timestamp, bool isEC);
    std::pair<std::unique_ptr<Buffer>, std::unique_ptr<Buffer>> HandleOutput(int &playbackScaledDuration);

    bool haveNext(bool ec);

    void Tick();
    void GetAverageLateCount(double *out);
    int GetAndResetLostPacketCount();
    double GetLastMeasuredJitter();
    double GetLastMeasuredDelay();

    double GetTimeoutWindow();

private:
    Mutex mutex;
    int64_t lastMain = 0;
    JitterArray slotsMain{false};
    JitterArray slotsEc{true};

    uint32_t step;
    int32_t nextFetchTimestamp = 0; // What frame to read next (protected for GetSeqTooLate)
    std::atomic<double> minDelay{6};
    uint32_t minMinDelay;
    uint32_t maxMinDelay;
    uint32_t maxUsedSlots;
    uint32_t lastPutTimestamp;
    uint32_t lossesToReset;
    double resyncThreshold;
    unsigned int lostCount = 0;
    unsigned int lostSinceReset = 0;
    unsigned int gotSinceReset = 0;
    bool wasReset = true;
    bool needBuffering = true;
    HistoricBuffer<int, 64, double> delayHistory;
    HistoricBuffer<int, 64, double> lateHistory;
    bool adjustingDelay = false;
    unsigned int tickCount = 0;
    unsigned int latePacketCount = 0;
    unsigned int dontIncMinDelayFor = 0;
    unsigned int dontDecMinDelayFor = 0;
    int lostPackets = 0;
    double prevRecvTime = 0;
    double expectNextAtTimeMs = 0;
    HistoricBuffer<double, 64> deviationHistory;
    double lastMeasuredJitter = 0;
    double lastMeasuredDelay = 0;
    int outstandingDelayChange = 0;
    unsigned int dontChangeDelayFor = 0;
    double avgDelay = 0;
    bool first = true;
#ifdef TGVOIP_DUMP_JITTER_STATS
    FILE *dump;
#endif
};
} // namespace tgvoip

#endif //LIBTGVOIP_JITTERBUFFER_H
