#include "PacketManager.h"
#include "../../tools/logging.h"

using namespace tgvoip;
using namespace std;

PacketManager::PacketManager() : recentIncomingSeqs(MAX_RECENT_PACKETS) {}

void PacketManager::ackLocal(uint32_t ackId, uint32_t mask)
{
    peerAcks[0] = ackId;
    for (unsigned int i = 1; i <= 32; i++)
    {
        peerAcks[i] = (mask >> (32 - i)) & 1 ? ackId - i : 0;
    }
}
bool PacketManager::wasLocalAcked(uint32_t seq)
{
    if (seqgt(seq, peerAcks[0]))
        return false;

    uint32_t distance = peerAcks[0] - seq;
    if (distance >= 0 && distance <= 32)
    {
        return peerAcks[distance];
    }

    return false;
}

bool PacketManager::ackRemoteSeq(uint32_t ackId)
{
    // Duplicate and moving window check
    if (seqgt(ackId, lastRemoteSeq - MAX_RECENT_PACKETS))
    {
        if (find(recentIncomingSeqs.begin(), recentIncomingSeqs.end(), ackId) != recentIncomingSeqs.end())
        {
            LOGW("Received duplicated packet for seq %u", ackId);
            return false;
        }
        recentIncomingSeqs.push_front(ackId);
        recentIncomingSeqs.pop_back();

        if (seqgt(ackId, lastRemoteSeq))
            lastRemoteSeq = ackId;
    }
    else
    {
        LOGW("Packet %u is out of order and too late", ackId);
        return false;
    }
    return true;
}
uint32_t PacketManager::getRemoteAckMask()
{
    uint32_t acks = 0;
    uint32_t distance;
    for (const uint32_t &seq : recentIncomingSeqs)
    {
        if (!seq)
            break;
        distance = lastRemoteSeq - seq;
        if (distance > 0 && distance <= 32)
        {
            acks |= (1 << (32 - distance));
        }
    }
    return acks;
}