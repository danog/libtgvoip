//
// Created by Grishka on 19/03/2019.
//

#ifndef LIBTGVOIP_VIDEOPACKETSENDER_H
#define LIBTGVOIP_VIDEOPACKETSENDER_H

#include "../controller/protocol/packets/PacketSender.h"
#include "../tools/Buffers.h"
#include "../tools/MessageThread.h"
#include "../tools/threading.h"
#include "ScreamCongestionController.h"
#include <memory>
#include <stdint.h>
#include <vector>

namespace tgvoip
{
namespace video
{
class VideoSource;

class VideoPacketSender : public PacketSender
{
public:
    VideoPacketSender(VoIPController *controller, const std::shared_ptr<OutgoingVideoStream> &stream, VideoSource *videoSource);
    virtual ~VideoPacketSender();
    virtual void PacketAcknowledged(const RecentOutgoingPacket &packet) override;
    virtual void PacketLost(const RecentOutgoingPacket &packet) override;
    void SetSource(VideoSource *source);

    uint32_t GetBitrate()
    {
        return currentVideoBitrate;
    }

private:
    struct SentVideoFrame
    {
        uint32_t seq;
        uint32_t fragmentCount;
        std::vector<uint32_t> unacknowledgedPackets;
        uint32_t fragmentsInQueue;
    };

    void SendFrame(const Buffer &frame, uint32_t flags, uint32_t rotation);
    int GetVideoResolutionForCurrentBitrate();

    VideoSource *source = NULL;
    video::ScreamCongestionController videoCongestionControl;
    double firstVideoFrameTime = 0.0;
    uint32_t videoFrameCount = 0;
    std::vector<SentVideoFrame> sentVideoFrames;
    bool videoKeyframeRequested = false;
    uint32_t sendVideoPacketID = MessageThread::INVALID_ID;
    uint32_t videoPacketLossCount = 0;
    uint32_t currentVideoBitrate = 0;
    double lastVideoResolutionChangeTime = 0.0;
    double sourceChangeTime = 0.0;

    const std::shared_ptr<OutgoingVideoStream> &stream;

    std::vector<Buffer>
        packetsForFEC;
    size_t fecFrameCount = 0;
    uint32_t frameSeq = 0;
};
} // namespace video
} // namespace tgvoip

#endif //LIBTGVOIP_VIDEOPACKETSENDER_H
