#pragma once
#include "../../tools/Buffers.h"
#include "../PrivateDefines.h"
#include "protocol/Extra.h"
#include "protocol/Interface.h"
//#include "../net/PacketSender.h"

namespace tgvoip
{
class PacketSender;

struct Packet : public Serializable, SingleChoice<Packet>
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

private:
    bool parseLegacy(const BufferInputStream &in, const VersionInfo &ver);
    bool parseLegacyLegacy(const BufferInputStream &in, unsigned char &type, uint32_t &ackId, uint32_t &pseq, uint32_t &acks, unsigned char &pflags, size_t &packetInnerLen, int peerVersion);

public:
    void serializeLegacy(std::vector<std::pair<unsigned char *, size_t>> &out, const VersionInfo &ver, const int state, const unsigned char *callID);

private:
    void writePacketHeaderLegacy(BufferOutputStream &out, const VersionInfo &ver, const uint32_t seq, const uint32_t ackSeq, const uint32_t ackMask, const unsigned char type, const std::vector<Wrapped<Extra>> &extras);
    void writePacketHeaderLegacyLegacy(BufferOutputStream &out, const VersionInfo &ver, const uint32_t pseq, const uint32_t ackSeq, const uint32_t ackMask, const unsigned char type, const uint32_t length, const std::vector<Wrapped<Extra>> &extras, const int state, const unsigned char *callID);

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
        return data || extraEC || extraSignaling || seq;
    }
};

// Legacy stuff
struct RecentOutgoingPacket
{
    // For simple NACK reliable resending
    int64_t endpoint;
    Buffer data;

    uint32_t seq;
    uint16_t id; // for group calls only
    double sendTime;
    double ackTime;
    double rttTime;
    uint8_t type;
    uint32_t size;
    PacketSender *sender;
    bool lost;
};
struct UnacknowledgedExtraData
{
    unsigned char type;
    Buffer data;
    uint32_t firstContainingSeq;
};
struct ReliableOutgoingPacket
{
    Buffer data;
    unsigned char type;
    HistoricBuffer<uint32_t, 16> seqs;
    double firstSentTime;
    double lastSentTime;
    double retryInterval;
    double timeout;
    uint8_t tries;
};
struct PendingOutgoingPacket
{
    PendingOutgoingPacket(uint32_t seq_, uint8_t type_, size_t len_, Buffer &&data_, int64_t endpoint_)
        : seq(seq_),
          type(type_),
          len(len_),
          data(std::move(data_)),
          endpoint(endpoint_)
    {
    }
    PendingOutgoingPacket(PendingOutgoingPacket &&other)
        : seq(other.seq),
          type(other.type),
          len(other.len),
          data(std::move(other.data)),
          endpoint(other.endpoint)
    {
    }
    PendingOutgoingPacket &operator=(PendingOutgoingPacket &&other)
    {
        if (this != &other)
        {
            seq = other.seq;
            type = other.type;
            len = other.len;
            data = std::move(other.data);
            endpoint = other.endpoint;
        }
        return *this;
    }
    TGVOIP_DISALLOW_COPY_AND_ASSIGN(PendingOutgoingPacket);
    uint32_t seq;
    uint8_t type;
    size_t len;
    Buffer data;
    int64_t endpoint;
};

/*
struct DebugLoggedPacket
{
    int32_t seq;
    double timestamp;
    int32_t length;
};
*/
} // namespace tgvoip