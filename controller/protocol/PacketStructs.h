#pragma once
#include "../PrivateDefines.h"
#include "../../tools/Buffers.h"
//#include "../net/PacketSender.h"

namespace tgvoip
{
class PacketSender;
struct Packet
{
    uint32_t seq;
    uint32_t ackSeq;
    uint32_t ackMask;

    uint8_t streamId;
    uint8_t flags;

    Buffer data;
    uint8_t eFlags = 0;
};
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