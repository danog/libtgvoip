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
class AudioPacketSender;
namespace video
{
class VideoPacketSender;
}

template <class T = PacketSender>
struct OutgoingStream : public StreamInfo
{
    OutgoingStream() = delete;
    OutgoingStream(uint8_t _id, StreamType _type);

    PacketManager packetManager;

    std::shared_ptr<T> packetSender;
};

struct OutgoingAudioStream : public AudioStreamInfo, public OutgoingStream<AudioPacketSender>
{
    OutgoingAudioStream(uint8_t _id = Packet::StreamId::Audio) : OutgoingStream(_id, TYPE){};

    static const StreamType TYPE = StreamType::Audio;
    static const bool OUTGOING = true;
};
struct IncomingAudioStream : public AudioStreamInfo, public IncomingStream
{
    IncomingAudioStream(uint8_t _id = Packet::StreamId::Audio) : IncomingStream(_id, TYPE){};

    std::shared_ptr<JitterBuffer> jitterBuffer;
    std::shared_ptr<tgvoip::OpusDecoder> decoder;

    std::shared_ptr<CallbackWrapper> callbackWrapper;

    static const StreamType TYPE = StreamType::Audio;
    static const bool OUTGOING = false;
};

struct OutgoingVideoStream : public VideoStreamInfo, public OutgoingStream<video::VideoPacketSender>
{
    OutgoingVideoStream(uint8_t _id = Packet::StreamId::Video) : OutgoingStream(_id, TYPE){};

    static const StreamType TYPE = StreamType::Video;
    static const bool OUTGOING = true;
};
struct IncomingVideoStream : public VideoStreamInfo, public IncomingStream
{
    IncomingVideoStream(uint8_t _id = Packet::StreamId::Video) : IncomingStream(_id, TYPE){};

    std::shared_ptr<PacketReassembler> packetReassembler;
    std::vector<Buffer> codecSpecificData;
    bool csdIsValid = false;

    static const StreamType TYPE = StreamType::Video;
    static const bool OUTGOING = false;
};

class PacketSender
{
public:
    PacketSender(VoIPController *_controller, const std::shared_ptr<OutgoingStream<>> &_stream);
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
    std::shared_ptr<OutgoingStream<>> stream;
    PacketManager &packetManager;

    //std::vector<PendingOutgoingPacket> reliableQueue;
};
} // namespace tgvoip
