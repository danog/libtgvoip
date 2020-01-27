#pragma once
#include <atomic>
#include <list>
#include "../PrivateDefines.h"
#include "PacketStructs.h"

namespace tgvoip
{
// Local and remote packet history management
struct Ack
{
    Ack();

    /* Local seqno ack */
    // Ack specified local seq + up to 32 seqs ago, specified by mask
    void ackLocal(uint32_t ackId, uint32_t mask);

    // Check if local seq was acked
    bool wasLocalAcked(uint32_t seq);

    // Get next local seqno
    inline uint32_t nextLocalSeq()
    {
        return seq++;
    }

    // Stream-specific local seqno
    std::atomic<uint32_t> seq = ATOMIC_VAR_INIT(1);

    // Recent ougoing packets
    std::vector<RecentOutgoingPacket> recentOutgoingPackets;

    // Seqno of last sent local packet
    uint32_t lastSentSeq = 0;

    // Status list of acked local seqnos, starting from the seq explicitly present in the packet + up to 32 seqs ago
    std::array<uint32_t, 33> peerAcks{0};

    /* Remote seqno ack */
    // Ack specified remote seq, returns false if too old or dupe
    bool ackRemoteSeq(uint32_t ackId);

    // Get ack mask for remote packets
    uint32_t getRemoteAckMask();

    // Seqno of last received remote packet
    uint32_t lastRemoteSeq = 0;

private: // Slowly encapsulate all the things
    // Recent incoming remote packets
    std::list<uint32_t> recentIncomingSeqs;
};
} // namespace tgvoip