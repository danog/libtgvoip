//
// Created by Grishka on 19/03/2019.
//
#pragma once

//#include "../../../VoIPController.h"
#include "../../audio/OpusDecoder.h"
#include "../../net/JitterBuffer.h"
#include "../Stream.h"
#include "PacketManager.h"
#include "PacketStructs.h"
#include <functional>
#include <stdint.h>

namespace tgvoip
{
class VoIPController;
class AudioPacketSender;
class PacketReassembler;
namespace video
{
class VideoPacketSender;
}
class PacketManager;
class PacketSender;

struct OutgoingStream : public StreamInfo
{
    OutgoingStream() = delete;
    OutgoingStream(uint8_t _id, StreamType _type);
    virtual ~OutgoingStream();

    virtual ExtraStreamInfo getStreamInfo() const;
    
    PacketManager packetManager;

    std::unique_ptr<PacketSender> packetSender;

    static const bool OUTGOING = true;
};
struct IncomingStream : public StreamInfo
{
    IncomingStream() = delete;
    IncomingStream(uint8_t id, StreamType type) : StreamInfo(id, type){};
    virtual ~IncomingStream() = default;

    static const bool OUTGOING = false;
};

struct OutgoingMediaStream : public MediaStreamInfo, public OutgoingStream
{
    OutgoingMediaStream() = delete;
    OutgoingMediaStream(uint8_t id, StreamType type);
    virtual ~OutgoingMediaStream();

    virtual ExtraStreamInfo getStreamInfo() const override;

    static const bool OUTGOING = true;
};

struct IncomingMediaStream : public MediaStreamInfo, public IncomingStream
{
    IncomingMediaStream() = delete;
    IncomingMediaStream(uint8_t id, StreamType type) : IncomingStream(id, type){};
    virtual ~IncomingMediaStream() = default;

    static const bool OUTGOING = false;
};

struct OutgoingAudioStream : public AudioStreamInfo,
                             public OutgoingMediaStream
{
    OutgoingAudioStream(uint8_t _id = StreamId::Audio);
    virtual ~OutgoingAudioStream();

    virtual ExtraStreamInfo getStreamInfo() const override;

    static const StreamType TYPE = StreamType::Audio;
    static const bool OUTGOING = true;
};
struct IncomingAudioStream : public AudioStreamInfo, public IncomingMediaStream
{
    IncomingAudioStream(uint8_t _id = StreamId::Audio) : IncomingMediaStream(_id, TYPE){};
    virtual ~IncomingAudioStream() = default;

    std::shared_ptr<JitterBuffer> jitterBuffer;
    std::shared_ptr<tgvoip::OpusDecoder> decoder;

    std::shared_ptr<CallbackWrapper> callbackWrapper;

    static const StreamType TYPE = StreamType::Audio;
    static const bool OUTGOING = false;
};

struct OutgoingVideoStream : public VideoStreamInfo, public OutgoingMediaStream
{
    OutgoingVideoStream(uint8_t _id = StreamId::Video);
    virtual ~OutgoingVideoStream();

    static const StreamType TYPE = StreamType::Video;
    static const bool OUTGOING = true;
};
struct IncomingVideoStream : public VideoStreamInfo, public IncomingMediaStream
{
    IncomingVideoStream(uint8_t _id = StreamId::Video) : IncomingMediaStream(_id, TYPE){};
    virtual ~IncomingVideoStream() = default;

    std::shared_ptr<PacketReassembler> packetReassembler;

    static const StreamType TYPE = StreamType::Video;
    static const bool OUTGOING = false;
};

class PacketSender
{
public:
    PacketSender(VoIPController *_controller, const std::shared_ptr<OutgoingStream> &_stream);
    virtual ~PacketSender() = default;

    virtual void PacketAcknowledged(const RecentOutgoingPacket &packet){};
    virtual void PacketLost(const RecentOutgoingPacket &packet){};

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
    std::shared_ptr<OutgoingStream> stream;
    PacketManager &packetManager;

    //std::vector<PendingOutgoingPacket> reliableQueue;
};
} // namespace tgvoip
