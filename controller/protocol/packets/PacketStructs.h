#pragma once
#include "../../../tools/Buffers.h"
#include "../../PrivateDefines.h"
#include "../../net/CongestionControl.h"
#include "../protocol/Extra.h"
#include "../protocol/Interface.h"
#include <vector>
//#include "../net/PacketSender.h"

namespace tgvoip
{
class PacketSender;
class PacketManager;
struct OutgoingPacket
{
    OutgoingPacket(Packet &&packet_, int64_t endpoint_ = 0)
        : packet(std::move(packet_)),
          endpoint(endpoint_)
    {
    }
    TGVOIP_MOVE_ONLY(OutgoingPacket);
    TGVOIP_DISALLOW_COPY_AND_ASSIGN(OutgoingPacket);
    Packet packet;
    int64_t endpoint = 0;
};
struct PendingOutgoingPacket
{
    PendingOutgoingPacket(std::shared_ptr<Buffer> &&_packet, CongestionControlPacket &&_pktInfo, int64_t _endpoint) : packet(std::move(_packet)),
                                                                                                                      pktInfo(std::move(pktInfo)),
                                                                                                                      endpoint(_endpoint){};
    //TGVOIP_MOVE_ONLY(PendingOutgoingPacket);
    //TGVOIP_DISALLOW_COPY_AND_ASSIGN(PendingOutgoingPacket);
    std::shared_ptr<Buffer> packet;
    CongestionControlPacket pktInfo;
    int64_t endpoint = 0;
};
struct ReliableOutgoingPacket
{
    PendingOutgoingPacket pkt;
    double firstSentTime;
    double lastSentTime;
    double retryInterval;
    double timeout;
    uint8_t tries;
};

struct RecentOutgoingPacket
{
    RecentOutgoingPacket(const PendingOutgoingPacket &_pkt) : pkt(_pkt.pktInfo),
                                                              size(_pkt.packet->Length()),
                                                              endpoint(_pkt.endpoint),
                                                              sendTime(VoIPController::GetCurrentTime()){};
    CongestionControlPacket pkt;
    size_t size;

    int64_t endpoint;

    uint16_t id; // for group calls only

    double sendTime;
    double ackTime = 0.0;
    double rttTime = 0.0;
    bool lost = false;
};
struct UnacknowledgedExtraData
{
    UnacknowledgedExtraData(Wrapped<Extra> &&data_, int64_t _endpointId = 0)
        : data(std::move(data_)),
          endpointId(_endpointId)
    {
    }

    TGVOIP_DISALLOW_COPY_AND_ASSIGN(UnacknowledgedExtraData);

    Wrapped<Extra> data;
    uint32_t firstContainingSeq = 0;
    int64_t endpointId = 0;
};
struct Packet : public Serializable, SingleChoice<Packet>
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

private:
    bool parseLegacy(const BufferInputStream &in, const VersionInfo &ver);
    bool parseLegacyLegacy(const BufferInputStream &in, unsigned char &type, uint32_t &ackId, uint32_t &pseq, uint32_t &acks, unsigned char &pflags, size_t &packetInnerLen, int peerVersion);

public:
    void serializeLegacy(std::vector<std::tuple<unsigned char *, size_t, bool>> &out, const VersionInfo &ver, const int state, const unsigned char *callID);

private:
    void writePacketHeaderLegacy(BufferOutputStream &out, const VersionInfo &ver, const uint32_t seq, const uint32_t ackSeq, const uint32_t ackMask, const unsigned char type, const std::vector<Wrapped<Extra>> &extras);
    void writePacketHeaderLegacyLegacy(BufferOutputStream &out, const VersionInfo &ver, const uint32_t pseq, const uint32_t ackSeq, const uint32_t ackMask, const unsigned char type, const uint32_t length, const std::vector<Wrapped<Extra>> &extras, const int state, const unsigned char *callID);

public:
    void prepare(PacketManager &pm);
    void prepare(PacketManager &pm, std::vector<UnacknowledgedExtraData> &currentExtras, const int64_t &endpointId);
    void prepare(PacketManager &pm, std::vector<UnacknowledgedExtraData> &currentExtras, const int64_t &endpointId, PacketManager &legacyPm);

public:
    enum Flags : uint8_t
    {
        Len16 = 1,
        RecvTS = 2,
        ExtraEC = 4,
        ExtraSignaling = 8
    };
    enum EFlags : uint8_t
    {
        Fragmented = 1,
        Keyframe = 2
    };

    enum StreamId : uint8_t
    {
        Signaling = 0,
        Audio = 1,
        Video = 2,
        Extended = 3
    };

    bool legacy = false;
    uint32_t legacySeq = 0;

    bool nopPacket = true;

    uint32_t seq = 0;
    uint32_t ackSeq = 0;
    uint32_t ackMask = 0;

    uint8_t streamId = 0;
    uint8_t eFlags = 0;

    uint8_t fragmentIndex = 0;
    uint8_t fragmentCount = 1;

    uint32_t recvTS = 0;

    Buffer data;

    Mask<Wrapped<Bytes>> extraEC;
    Array<Wrapped<Extra>> extraSignaling;

    // Ugly backwards compatibility hacks
    std::vector<Packet> otherPackets; // parse
public:
    operator bool()
    {
        return data || extraEC || extraSignaling || nopPacket;
    }
    std::string print() const override
    {
        std::stringstream res;
        res << data ? "Data packet" : extraEC ? "EC packet" : extraSignaling ? "Signaling packet" : nopPacket ? "NOP packet" : "Empty packet";
        res << "(streamId=" << streamId << ")";
        if (extraEC)
            res << "; extraEC";
        if (extraSignaling)
            res << "; signaling " + extraSignaling.print();
        return res.str();
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        if (!ver.isNew())
            return 0; // Don't even try

        return sizeof(seq) +
               sizeof(ackSeq) +
               sizeof(ackMask) +
               sizeof(streamId) +
               (streamId > StreamId::Extended ? sizeof(streamId) : 0) +
               (data.Length() > 0xFF || eFlags ? 2 : 1) + // Length
               (recvTS ? sizeof(recvTS) : 0) +
               (extraEC ? extraEC.getSize(ver) : 0) +
               (extraSignaling ? extraSignaling.getSize(ver) : 0);
    }
};

using StreamId = Packet::StreamId;

/*
struct DebugLoggedPacket
{
    int32_t seq;
    double timestamp;
    int32_t length;
};
*/
} // namespace tgvoip