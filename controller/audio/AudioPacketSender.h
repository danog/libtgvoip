#pragma once
#include "../protocol/packets/PacketSender.h"
#include <list>

namespace tgvoip
{
class VoIPController;
class AudioPacketSender : public PacketSender
{
    friend class VoIPController;

public:
    AudioPacketSender(VoIPController *controller, const std::shared_ptr<AudioStream> &stream, const std::shared_ptr<OpusEncoder> &encoder);
    virtual ~AudioPacketSender() = default;
    virtual void PacketAcknowledged(const RecentOutgoingPacket &packet) override{};
    virtual void PacketLost(const RecentOutgoingPacket &packet) override{};
    void SetSource(const std::shared_ptr<OpusEncoder> &encoder);

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
    void setAudioPreprocDataCallback(const std::function<void(int16_t *, size_t)> &audioPreprocDataCallback)
    {
        this->audioPreprocDataCallback = audioPreprocDataCallback;
    }
#endif

    inline const bool getShittyInternetMode() const
    {
        return shittyInternetMode;
    }
    inline const uint8_t getExtraEcLevel() const
    {
        return extraEcLevel;
    }

    inline void setShittyInternetMode(bool shittyInternetMode)
    {
        this->shittyInternetMode = shittyInternetMode;
    }
    inline void setExtraEcLevel(uint8_t extraEcLevel)
    {
        this->extraEcLevel = extraEcLevel;
    }

    double setPacketLoss(double percent);

private:
    void SendFrame(unsigned char *data, size_t len, unsigned char *secondaryData, size_t secondaryLen);

    std::shared_ptr<OpusEncoder> encoder;

    uint32_t audioTimestampOut = 0;

    bool shittyInternetMode = false;
    uint8_t extraEcLevel = 0;

    double packetLoss = 0.0;

    double resendCount = 1.0;

    std::deque<Buffer> ecAudioPackets;

    BufferPool<1024, 32> outgoingAudioBufferPool;

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
    std::function<void(int16_t *, size_t)> audioPreprocDataCallback;
    std::unique_ptr<::OpusDecoder, decltype(&opus_decoder_destroy)> preprocDecoder{opus_decoder_create(48000, 1, NULL), &opus_decoder_destroy};
    int16_t preprocBuffer[4096];
#endif
};
} // namespace tgvoip