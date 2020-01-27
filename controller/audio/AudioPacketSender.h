#pragma once
#include "../net/PacketSender.h"

namespace tgvoip
{
class AudioPacketSender : PacketSender
{
    AudioPacketSender(VoIPController *controller, VideoSource *videoSource, std::shared_ptr<VoIPController::Stream> stream);
    virtual ~AudioPacketSender();
    virtual void PacketAcknowledged(uint32_t seq, double sendTime, double ackTime, uint8_t type, uint32_t size) override;
    virtual void PacketLost(uint32_t seq, uint8_t type, uint32_t size) override;
    void SetSource(VideoSource *source);
}
} // namespace tgvoip