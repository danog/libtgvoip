#include "PacketManager.h"
#include "../../tools/logging.h"

using namespace tgvoip;
using namespace std;

void PacketManager::ackLocal(uint32_t ackId, uint32_t mask)
{
    lastAckedSeq = ackId;
    lastAckedSeqsMask = mask;
}
bool PacketManager::wasLocalAcked(uint32_t seq)
{
    if (seq == lastAckedSeq)
        return true;
    if (seq > lastAckedSeq)
        return false;

    uint32_t distance = lastAckedSeq - seq;
    if (distance > 0 && distance <= 32)
    {
        return (lastAckedSeqsMask >> (32 - distance)) & 1;
    }

    return false;
}

bool PacketManager::ackRemoteSeq(uint32_t ackId)
{
    if (seqgt(ackId, lastRemoteSeq - MAX_RECENT_PACKETS))
    {
        if (ackId == lastRemoteSeq)
        {
            LOGW("Received duplicated packet for seq %u", ackId);
            return false;
        }
        else if (ackId > lastRemoteSeq)
        {
            lastRemoteSeqsMask = (lastRemoteSeqsMask << 1) | 1;
            lastRemoteSeqsMask <<= (ackId - lastRemoteSeq) - 1;
            lastRemoteSeq = ackId;
        }
        else
        {
            uint32_t pos = 1 << ((lastRemoteSeq - ackId) - 1);
            if (lastRemoteSeqsMask & pos)
            {
                LOGW("Received duplicated packet for seq %u", ackId);
                return false;
            }
            lastRemoteSeqsMask |= pos;
        }
    }
    else
    {
        LOGW("Packet %u is out of order and too late", ackId);
        return false;
    }
    return true;
}