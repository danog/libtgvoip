//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_JITTERBUFFER_H
#define LIBTGVOIP_JITTERBUFFER_H

#include <stdlib.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <stdio.h>
#include "controller/media/MediaStreamItf.h"
#include "tools/BlockingQueue.h"
#include "tools/Buffers.h"
#include "tools/threading.h"
#include "tools/logging.h"

#define JITTER_SLOT_COUNT 64
#define JITTER_SLOT_SIZE 1024
#define JR_OK 1
#define JR_MISSING 2
#define JR_BUFFERING 3

namespace tgvoip
{
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
	void HandleInput(unsigned char *data, size_t len, uint32_t timestamp, bool isEC);
	size_t HandleOutput(unsigned char *buffer, size_t len, int offsetInSteps, bool advance, int &playbackScaledDuration, bool &isEC);
	void Tick();
	void GetAverageLateCount(double *out);
	int GetAndResetLostPacketCount();
	double GetLastMeasuredJitter();
	double GetLastMeasuredDelay();

	double GetTimeoutWindow();

	// Get minimum refetchable seq for (reverse) NACK logic.
	// Any sequence numbers smaller than this cannot possibly arrive in time for playing.
	inline uint32_t GetSeqTooLate(double rtt)
	{
		//LOGE("Next fetch timestamp: %ld, rtt %lf, step %d", nextFetchTimestamp.load(), rtt * 1000, step)
		// The absolute minimum time(stamp) that will (barely) be accepted by the jitter buffer in time + RTT time
		// Then convert timestamp into a seqno: remember, in protocol >= PROTOCOL_RELIABLE, seq = ts * step + 1
		return ((nextFetchTimestamp + (rtt * 1000)) / static_cast<uint64_t>(step) + 1) - lostCount; // Seqs start at 1
	}

private:
	struct jitter_packet_t
	{
		Buffer buffer = Buffer();
		size_t size;
		uint32_t timestamp;
		bool isEC;
		double recvTimeDiff;
	};
	void PutInternal(jitter_packet_t *pkt, bool overwriteExisting);
	int GetInternal(jitter_packet_t *pkt, int offset, bool advance);
	void Advance();

	BufferPool<JITTER_SLOT_SIZE, JITTER_SLOT_COUNT> bufferPool;
	Mutex mutex;
	uint32_t step;
	std::array<jitter_packet_t, JITTER_SLOT_COUNT> slots;
	std::atomic<int64_t> nextFetchTimestamp = ATOMIC_VAR_INIT(0); // What frame to read next
	double minDelay = 6;
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
	unsigned int dontIncMinDelay = 0;
	unsigned int dontDecMinDelay = 0;
	int lostPackets = 0;
	double prevRecvTime = 0;
	double expectNextAtTime = 0;
	HistoricBuffer<double, 64> deviationHistory;
	double lastMeasuredJitter = 0;
	double lastMeasuredDelay = 0;
	int outstandingDelayChange = 0;
	unsigned int dontChangeDelay = 0;
	double avgDelay = 0;
	bool first = true;
#ifdef TGVOIP_DUMP_JITTER_STATS
	FILE *dump;
#endif
};
} // namespace tgvoip

#endif //LIBTGVOIP_JITTERBUFFER_H
