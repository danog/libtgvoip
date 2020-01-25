//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_JITTERBUFFER_H
#define LIBTGVOIP_JITTERBUFFER_H

#include <stdlib.h>
#include <vector>
#include <stdio.h>
#include "controller/media/MediaStreamItf.h"
#include "tools/BlockingQueue.h"
#include "tools/Buffers.h"
#include "tools/threading.h"

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
	int64_t nextTimestamp = 0;
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
