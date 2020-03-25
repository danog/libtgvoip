#include "PacketManager.h"
#include "PacketStructs.h"
#include "../../../tools/logging.h"
#include "../../../VoIPController.h"

using namespace tgvoip;


PacketManager::PacketManager(uint8_t transportId) : transportId(transportId)
{
    recentOutgoingPackets.reserve(MAX_RECENT_PACKETS);
}

void PacketManager::ackLocal(uint32_t ackId, uint32_t mask)
{
    lastAckedSeq = ackId;
    lastAckedSeqsMask = mask;
}
bool PacketManager::wasLocalAcked(uint32_t seq) const
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

bool PacketManager::ackRemoteSeq(const Packet &pkt)
{
    if (pkt.legacy) {
        if (pkt.legacySeq) {
            return ackRemoteSeq(pkt.legacySeq);
        }
    } else {
        return ackRemoteSeq(pkt.seq);
    }
    return true;
}

bool PacketManager::ackRemoteSeq(const uint32_t ackId)
{
    if (seqgt(ackId, lastRemoteSeq - MAX_RECENT_PACKETS))
    {
        if (ackId == lastRemoteSeq)
        {
            LOGW("Received duplicated packet for seq %u, streamId=%hhu", ackId, transportId);
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
                LOGW("Received duplicated packet for seq %u, streamId=%hhu", ackId, transportId);
                return false;
            }
            lastRemoteSeqsMask |= pos;
        }
    }
    else
    {
        LOGW("Packet %u, streamId=%hhu is out of order and too late", ackId, transportId);
        return false;
    }
    return true;
}

std::vector<RecentOutgoingPacket> &PacketManager::getRecentOutgoingPackets()
{
    return recentOutgoingPackets;
}
void PacketManager::addRecentOutgoingPacket(const PendingOutgoingPacket &pkt)
{
    addRecentOutgoingPacket(RecentOutgoingPacket(pkt, VoIPController::GetCurrentTime()));
}
void PacketManager::addRecentOutgoingPacket(RecentOutgoingPacket &&pkt)
{
    recentOutgoingPackets.push_back(std::move(pkt));
    while (recentOutgoingPackets.size() > MAX_RECENT_PACKETS)
    {
        recentOutgoingPackets.erase(recentOutgoingPackets.begin());
    }
    lastSentSeq = pkt.pkt.seq;
}
