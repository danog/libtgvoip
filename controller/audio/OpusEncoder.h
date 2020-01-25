//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_OPUSENCODER_H
#define LIBTGVOIP_OPUSENCODER_H

#include "controller/media/MediaStreamItf.h"
#include "tools/threading.h"
#include "tools/BlockingQueue.h"
#include "tools/Buffers.h"
#include "controller/audio/EchoCanceller.h"
#include "tools/utils.h"

#include <stdint.h>
#include <atomic>

struct OpusEncoder;

namespace tgvoip
{
class OpusEncoder
{
public:
	TGVOIP_DISALLOW_COPY_AND_ASSIGN(OpusEncoder);
	OpusEncoder(const std::shared_ptr<MediaStreamItf> &source, bool needSecondary);
	virtual ~OpusEncoder();
	virtual void Start();
	virtual void Stop();
	void SetBitrate(uint32_t bitrate);
	void SetEchoCanceller(const std::shared_ptr<EchoCanceller> &aec);
	void SetOutputFrameDuration(uint32_t duration);
	void SetPacketLoss(int percent);
	int GetPacketLoss();
	uint32_t GetBitrate();
	void SetDTX(bool enable);
	void SetLevelMeter(const std::shared_ptr<AudioLevelMeter> &levelMeter);
	void SetCallback(std::function<void(unsigned char *, size_t, unsigned char *, size_t)> callback);
	void SetSecondaryEncoderEnabled(bool enabled);
	void SetVadMode(bool vad);
	void AddAudioEffect(const std::shared_ptr<effects::AudioEffect> &effect);
	void RemoveAudioEffect(const std::shared_ptr<effects::AudioEffect> &effect);
	int GetComplexity()
	{
		return complexity;
	}

private:
	static size_t Callback(unsigned char *data, size_t len, void *param);
	void RunThread();
	void Encode(int16_t *data, size_t len);
	void InvokeCallback(unsigned char *data, size_t length, unsigned char *secondaryData, size_t secondaryLength);
	std::shared_ptr<MediaStreamItf> source;
	::OpusEncoder *enc;
	::OpusEncoder *secondaryEncoder;
	unsigned char buffer[4096];
	std::atomic<uint32_t> requestedBitrate;
	uint32_t currentBitrate;
	Thread *thread;
	BlockingQueue<Buffer> queue;
	BufferPool<960 * 2, 10> bufferPool;
	std::shared_ptr<EchoCanceller> echoCanceller;
	std::atomic<int> complexity;
	std::atomic<bool> running;
	uint32_t frameDuration;
	int packetLossPercent;
	std::shared_ptr<AudioLevelMeter> levelMeter;
	bool secondaryEncoderEnabled;
	bool vadMode = false;
	uint32_t vadNoVoiceBitrate;
	std::vector<std::shared_ptr<effects::AudioEffect>> postProcEffects;
	int secondaryEnabledBandwidth;
	int vadModeVoiceBandwidth;
	int vadModeNoVoiceBandwidth;

	bool wasSecondaryEncoderEnabled = false;

	std::function<void(unsigned char *, size_t, unsigned char *, size_t)> callback;
};
} // namespace tgvoip

#endif //LIBTGVOIP_OPUSENCODER_H
