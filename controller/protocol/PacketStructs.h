#pragma once
#include "../PrivateDefines.h"
#include "../../tools/Buffers.h"
//#include "../net/PacketSender.h"

namespace tgvoip
{
class PacketSender;
struct Serializable
{
    virtual bool parse(const BufferInputStream &in, int peerVersion) = 0;
    virtual bool serialize(BufferOutputStream &out, int peerVersion) = 0;
};
template <typename T>
struct Array
{
public:
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    std::vector<std::shared_ptr<T>> contents;
};

struct Extra : public Serializable
{
    static std::shared_ptr<Extra> choose(const BufferInputStream &in, int peerVersion);
};
struct Packet : public Serializable
{
public:
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

private:
    bool parseLegacyPacket(const BufferInputStream &in, int peerVersion);
    bool parseLegacyLegacyPacket(const BufferInputStream &in, unsigned char &type, uint32_t &ackId, uint32_t &pseq, uint32_t &acks, unsigned char &pflags, size_t &packetInnerLen, int peerVersion);

public:
    enum Flags : uint8_t
    {
        Len16 = 1,
        ExtraFEC = 2,
        ExtraSignaling = 4,
        RecvTS = 8
    };
    enum EFlags : uint8_t
    {
        Fragmented = 1,
        Keyframe = 2
    };

    uint32_t seq;
    uint32_t ackSeq;
    uint32_t ackMask;

    uint8_t streamId;
    uint8_t flags = 0;
    uint8_t eFlags = 0;

    uint32_t recvTS = 0;

    Buffer data;

    std::vector<std::shared_ptr<Extra>> extras;
};

struct StreamInfo : public Serializable
{
public:
    static std::shared_ptr<StreamInfo> choose(const BufferInputStream &in, int peerVersion)
    {
        return std::make_shared<StreamInfo>();
    };

    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    uint8_t streamId = 0;
    uint8_t type = 0;
    uint32_t codec = 0;
    uint16_t frameDuration = 0;
    uint8_t enabled = 0;
};

struct ExtraStreamFlags : public Extra
{
public:
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    enum Flags : uint8_t
    {
        Enabled = 1,
        Dtx = 2,
        ExtraEC = 3,
        Paused = 4
    };

    uint8_t flags = 0;

    static const uint8_t ID = 1;
};

struct ExtraStreamCsd : public Extra
{
public:
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    uint16_t width = 0;
    uint16_t height = 0;

    std::vector<BufferInputStream> data;

    static const uint8_t ID = 2;
};

struct ExtraLanEndpoint : public Extra
{
public:
    virtual bool parse(const BufferInputStream &in, int peerVersion);
    virtual bool serialize(BufferOutputStream &out, int peerVersion);

    NetworkAddress address;
    uint16_t port = 0;

    static const uint8_t ID = 3;
};

struct ExtraIpv6Endpoint : public Extra
{
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    NetworkAddress address;
    uint16_t port = 0;

    static const uint8_t ID = 7;
};

struct ExtraNetworkChanged : public Extra
{
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    enum Flags : uint8_t
    {
        DataSavingEnabled = 1
    };

    uint8_t flags = 0;

    static const uint8_t ID = 4;
};

struct ExtraGroupCallKey : public Extra
{
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    std::array<uint8_t, 256> key;

    static const uint8_t ID = 5;
};

struct ExtraGroupCallUpgrade : public Extra
{
    bool parse(const BufferInputStream &in, int peerVersion) override{};
    bool serialize(BufferOutputStream &out, int peerVersion) override{};

    static const uint8_t ID = 6;
};

struct ExtraInit : public Extra
{
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    enum Flags : uint8_t
    {
        DataSavingEnabled = 1,
        GroupCallSupported = 2,
        VideoSendSupported = 4,
        VideoRecvSupported = 8
    };

    uint32_t peerVersion = 0;
    uint32_t minVersion = 0;
    uint8_t flags = 0;

    std::vector<std::shared_ptr<StreamInfo>> streams;

    static const uint8_t ID = 8;
};

struct ExtraInitAck : public Extra
{
    bool parse(const BufferInputStream &in, int peerVersion) override;
    bool serialize(BufferOutputStream &out, int peerVersion) override;

    uint32_t peerVersion = 0;
    uint32_t minVersion = 0;

    std::vector<std::shared_ptr<StreamInfo>> streams;

    static const uint8_t ID = 8;
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