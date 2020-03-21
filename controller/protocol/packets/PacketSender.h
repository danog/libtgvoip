//
// Created by Grishka on 19/03/2019.
//
#pragma once

//#include "../../../VoIPController.h"
#include "../Stream.h"
#include "PacketManager.h"
#include "PacketStructs.h"
#include <functional>
#include <stdint.h>

namespace tgvoip
{
class VoIPController;

struct Stream
{
    Stream() = delete;
    Stream(uint8_t _id, StreamInfo::Type _type);

    inline PacketManager &getPacketManager()
    {
        return packetManager;
    }

    uint8_t id;
    StreamInfo::Type type;
    PacketManager packetManager;

    std::unique_ptr<PacketSender> packetSender;

    bool enabled = true;
    bool paused = false;
};

struct MediaStream : public Stream
{
    MediaStream() = delete;
    MediaStream(uint8_t _id, StreamInfo::Type _type);

    int32_t userID;

    uint32_t codec;
    bool extraECEnabled;
    uint16_t frameDuration;
    std::shared_ptr<PacketReassembler> packetReassembler;
    std::shared_ptr<CallbackWrapper> callbackWrapper;
    std::vector<Buffer> codecSpecificData;
    bool csdIsValid = false;
};
struct AudioStream : public MediaStream
{
    AudioStream(uint8_t _id = Packet::StreamId::Audio) : MediaStream(_id, StreamInfo::Type::Audio){};

    std::shared_ptr<JitterBuffer> jitterBuffer;
    std::shared_ptr<tgvoip::OpusDecoder> decoder;
};
struct VideoStream : public MediaStream
{
    VideoStream(uint8_t _id = Packet::StreamId::Video) : MediaStream(_id, StreamInfo::Type::Video){};

    unsigned int width = 0;
    unsigned int height = 0;
    uint16_t rotation = 0;
    int resolution;
};

class PacketSender
{
public:
    PacketSender(VoIPController *_controller, const std::shared_ptr<Stream> &_stream);
    virtual ~PacketSender() = default;

    virtual void PacketAcknowledged(const RecentOutgoingPacket &packet) = 0;
    virtual void PacketLost(const RecentOutgoingPacket &packet) = 0;

protected:
    /*
    inline void SendExtra(Buffer &data, unsigned char type)
    {
        controller->SendExtra(data, type);
    }

    inline void IncrementUnsentStreamPackets()
    {
        controller->unsentStreamPackets++;
    }

    inline uint32_t SendPacket(PendingOutgoingPacket pkt)
    {
        uint32_t seq = controller->peerVersion < PROTOCOL_RELIABLE ? controller->packetManager.nextLocalSeq() : packetManager.nextLocalSeq();
        pkt.seq = seq;
        controller->SendOrEnqueuePacket(std::move(pkt), true, this);
        return seq;
    }

    inline void SendPacketReliably(unsigned char type, unsigned char *data, size_t len, double retryInterval, double timeout, uint8_t tries = 0xFF)
    {
        controller->SendPacketReliably(type, data, len, retryInterval, timeout, tries);
    }

    inline double GetConnectionInitTime()
    {
        return controller->connectionInitTime;
    }

    inline const HistoricBuffer<double, 32> &RTTHistory() const
    {
        return controller->rttHistory;
    }

    inline MessageThread &GetMessageThread()
    {
        return controller->messageThread;
    }

    inline void SendStreamFlags(const Stream &stm)
    {
        controller->SendStreamFlags(stm);
    }

    inline const VoIPController::Config &GetConfig() const
    {
        return controller->config;
    }
    inline const bool IsStopping() const
    {
        return controller->stopping;
    }
    inline const bool ReceivedInitAck() const
    {
        return controller->receivedInitAck;
    }

    inline auto &GetConctl()
    {
        return controller->conctl;
    }

    inline const int32_t PeerVersion() const
    {
        return controller->peerVersion;
    }

    inline const double LastRtt() const
    {
        return controller->rttHistory[0];
    }*/

    VoIPController *controller;
    std::shared_ptr<Stream> stream;
    PacketManager &packetManager;

    //std::vector<PendingOutgoingPacket> reliableQueue;
};
} // namespace tgvoip
