#pragma once
#include "PacketStructs.h"
#include <atomic>
#include <bitset>

namespace tgvoip
{

#define SEQ_MAX 0xFFFFFFFF

inline bool seqgt(uint32_t s1, uint32_t s2)
{
    return ((s1 > s2) && (s1 - s2 <= SEQ_MAX / 2)) || ((s1 < s2) && (s2 - s1 > SEQ_MAX / 2));
}

inline bool seqgte(uint32_t s1, uint32_t s2)
{
    return s1 == s2 || seqgt(s1, s2);
}

// Local and remote packet history management
class PacketManager final
{
public:
    PacketManager() = delete;
    PacketManager(uint8_t transportId);

    bool operator==(const PacketManager &other)
    {
        return transportId == other.transportId;
    }
    bool operator!=(const PacketManager &other)
    {
        return transportId != other.transportId;
    }
    // Transport ID for multiplexing
    inline uint8_t getTransportId()
    {
        return transportId;
    }

    uint8_t transportId;

public:
    /* Local seqno generation */

    // Get next local seqno
    inline uint32_t nextLocalSeq()
    {
        return seq++;
    }

    // Get current local seqno
    inline uint32_t getLocalSeq() const
    {
        return seq;
    }

    inline void setLocalSeq(uint32_t _seq)
    {
        seq = _seq;
    }

    inline uint32_t getLastSentSeq() onst
    {
        return lastSentSeq;
    }

    inline void setLastSentSeq(uint32_t lastSentSeq)
    {
        this->lastSentSeq = lastSentSeq;
    }

    // Seqno of last sent local packet
    uint32_t lastSentSeq = 0;

private:
    // Stream-specific local seqno
    std::atomic<uint32_t> seq = ATOMIC_VAR_INIT(1);

public:
    /* Local seqno acks */

    // Ack specified local seq + up to 32 seqs ago, specified by mask
    void ackLocal(uint32_t ackId, uint32_t mask);

    // Check if local seq was acked
    bool wasLocalAcked(uint32_t seq) const;

    inline uint32_t getLastAckedSeq() const
    {
        return lastAckedSeq;
    }

private:
    // Seqno of last acked packet
    uint32_t lastAckedSeq = 0;

    // Status list of acked local seqnos, excluding the seq explicitly present in the packet, up to 32 seqs ago
    uint32_t lastAckedSeqsMask = 0;

public:
    /* Remote seqno ack */
    // Ack specified remote seq, returns false if too old or dupe
    bool ackRemoteSeq(uint32_t ackId);

    // Ack remote seqs older than the specified seq
    inline void ackRemoteSeqsOlderThan(uint32_t seq)
    {
        lastRemoteSeqsMask |= 0xFFFFFFFF << (lastRemoteSeq - seq);
    }

    // Get ack mask for remote packets
    inline uint32_t getRemoteAckMask() const
    {
        return lastRemoteSeqsMask;
    }

    inline uint32_t getLastRemoteSeq() const
    {
        return lastRemoteSeq;
    }

private:
    // Seqno of last received remote packet
    uint32_t lastRemoteSeq = 0;

    // Recent incoming remote packets
    uint32_t lastRemoteSeqsMask;

public: // Recent outgoing packet list
    inline std::vector<RecentOutgoingPacket> &getRecentOutgoingPackets()
    {
        return recentOutgoingPackets;
    }

    RecentOutgoingPacket *GetRecentOutgoingPacket(uint32_t seq);

private:
    // Recent ougoing packets
    std::vector<RecentOutgoingPacket> recentOutgoingPackets;
};
} // namespace tgvoip