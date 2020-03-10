//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_OPUSDECODER_H
#define LIBTGVOIP_OPUSDECODER_H

#include "controller/media/MediaStreamItf.h"
#include "tools/threading.h"
#include "tools/BlockingQueue.h"
#include "tools/Buffers.h"
#include "controller/audio/EchoCanceller.h"
#include "controller/net/JitterBuffer.h"
#include "tools/utils.h"
#include <stdio.h>
#include <vector>
#include <memory>
#include <atomic>


struct OpusDecoder;

namespace tgvoip
{
class OpusDecoder
{
public:
	TGVOIP_DISALLOW_COPY_AND_ASSIGN(OpusDecoder);
	virtual void Start();

	virtual void Stop();

	OpusDecoder(const std::shared_ptr<MediaStreamItf> &dst, bool isAsync, bool needEC);
	OpusDecoder(const std::unique_ptr<MediaStreamItf> &dst, bool isAsync, bool needEC);
	virtual ~OpusDecoder();
	size_t HandleCallback(unsigned char *data, size_t len);
	void SetEchoCanceller(const std::shared_ptr<EchoCanceller> &canceller);
	void SetFrameDuration(uint32_t duration);
	void SetJitterBuffer(const std::shared_ptr<JitterBuffer> &jitterBuffer);
	void SetDTX(bool enable);
	void SetLevelMeter(const std::shared_ptr<AudioLevelMeter> &levelMeter);
	void AddAudioEffect(const std::shared_ptr<effects::AudioEffect> &effect);
	void RemoveAudioEffect(const std::shared_ptr<effects::AudioEffect> &effect);

private:
	void Initialize(bool isAsync, bool needEC);
	static size_t Callback(unsigned char *data, size_t len, void *param);
	void RunThread();
	int DecodeNextFrame();
	::OpusDecoder *dec;
	::OpusDecoder *ecDec;
	BlockingQueue<Buffer> *decodedQueue;
	BufferPool<960 * 2, 32> bufferPool;
	unsigned char *buffer;
	unsigned char *lastDecoded;
	unsigned char *processedBuffer;
	size_t outputBufferSize;
	std::atomic<bool> running;
	Thread *thread;
	Semaphore *semaphore;
	uint32_t frameDuration;
	std::shared_ptr<EchoCanceller> echoCanceller;
	std::shared_ptr<JitterBuffer> jitterBuffer;
	std::shared_ptr<AudioLevelMeter> levelMeter;
	int consecutiveLostPackets;
	bool enableDTX;
	size_t silentPacketCount;
	std::vector<std::shared_ptr<effects::AudioEffect>> postProcEffects;
	//bool async;
	std::atomic<bool> async;
	alignas(2) unsigned char nextBuffer[8192];
	alignas(2) unsigned char decodeBuffer[8192];
	size_t nextLen;
	unsigned int packetsPerFrame;
	ptrdiff_t remainingDataLen;
	bool prevWasEC;
	int16_t prevLastSample;
};
} // namespace tgvoip

#endif //LIBTGVOIP_OPUSDECODER_H
