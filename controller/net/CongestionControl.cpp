//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "CongestionControl.h"
#include "../PrivateDefines.h"
#include "../protocol/packets/PacketStructs.h"
#include "VoIPController.h"
#include "VoIPServerConfig.h"
#include "tools/logging.h"
#include <assert.h>
#include <math.h>

using namespace tgvoip;

CongestionControlPacket::CongestionControlPacket(uint32_t _seq, uint8_t _streamId) : seq(_seq), streamId(_streamId){};
CongestionControlPacket::CongestionControlPacket(const Packet &pkt) : seq(pkt.seq), streamId(pkt.streamId){};

CongestionControl::CongestionControl() : cwnd(static_cast<size_t>(ServerConfig::GetSharedInstance()->GetInt("audio_congestion_window", 1024))),
                                         max(cwnd * 1.1),
                                         min(cwnd * 0.9)
{
}

CongestionControl::~CongestionControl()
{
}

size_t CongestionControl::GetAcknowledgedDataSize()
{
    return 0;
}

double CongestionControl::GetAverageRTT()
{
    return rttHistory.NonZeroAverage();
}

size_t CongestionControl::GetInflightDataSize()
{
    return inflightHistory.Average();
}

size_t CongestionControl::GetCongestionWindow()
{
    return cwnd;
}

double CongestionControl::GetMinimumRTT()
{
    return rttHistory.Min();
}

void CongestionControl::PacketSent(const CongestionControlPacket &pkt, size_t size)
{
    if (lastSentSeq.find(pkt.streamId) == lastSentSeq.end())
    {
        lastSentSeq[pkt.streamId] = 0;
    }
    if (!seqgt(pkt.seq, lastSentSeq[pkt.streamId]) || pkt.seq == lastSentSeq[pkt.streamId])
    {
        LOGW("Duplicate outgoing seq %u", pkt.seq);
        return;
    }
    lastSentSeq[pkt.streamId] = pkt.seq;
    double smallestSendTime = INFINITY;
    tgvoip_congestionctl_packet_t *slot = NULL;
    for (auto &packet : inflightPackets)
    {
        if (packet.sendTime == 0)
        {
            slot = &packet;
            break;
        }
        if (smallestSendTime > packet.sendTime)
        {
            slot = &packet;
            smallestSendTime = slot->sendTime;
        }
    }
    assert(slot != NULL);
    if (slot->sendTime > 0)
    {
        inflightDataSize -= slot->size;
        lossCount++;
        LOGD("Packet with seq %u, streamId=%hhu was not acknowledged", slot->seq, slot->streamId);
    }
    slot->seq = pkt.seq;
    slot->size = size;
    slot->streamId = pkt.streamId;
    slot->sendTime = VoIPController::GetCurrentTime();
    inflightDataSize += size;
}

void CongestionControl::PacketAcknowledged(const CongestionControlPacket &pkt)
{
    for (auto &packet : inflightPackets)
    {
        if (packet.seq == pkt.seq && packet.streamId == pkt.streamId && packet.sendTime > 0)
        {
            tmpRtt += (VoIPController::GetCurrentTime() - packet.sendTime);
            tmpRttCount++;
            packet.sendTime = 0;
            inflightDataSize -= packet.size;
            break;
        }
    }
}

void CongestionControl::PacketLost(const CongestionControlPacket &pkt)
{
    for (auto &packet : inflightPackets)
    {
        if (packet.seq == pkt.seq && packet.streamId == pkt.streamId && packet.sendTime > 0)
        {
            packet.sendTime = 0;
            inflightDataSize -= packet.size;
            lossCount++;
            break;
        }
    }
}

void CongestionControl::Tick()
{
    tickCount++;
    if (tmpRttCount > 0)
    {
        rttHistory.Add(tmpRtt / tmpRttCount);
        tmpRtt = 0;
        tmpRttCount = 0;
    }
    for (auto &packet : inflightPackets)
    {
        if (packet.sendTime && VoIPController::GetCurrentTime() - packet.sendTime > TGVOIP_CONCTL_LOST_AFTER)
        {
            packet.sendTime = 0;
            inflightDataSize -= packet.size;
            lossCount++;
            LOGD("Packet with seq %u was not acknowledged", packet.seq);
        }
    }
    inflightHistory.Add(inflightDataSize);
}

int CongestionControl::GetBandwidthControlAction()
{
    if (VoIPController::GetCurrentTime() - lastActionTime < 1)
        return TGVOIP_CONCTL_ACT_NONE;

    size_t inflightAvg = GetInflightDataSize();
    if (inflightAvg < min)
    {
        lastActionTime = VoIPController::GetCurrentTime();
        return TGVOIP_CONCTL_ACT_INCREASE;
    }
    if (inflightAvg > max)
    {
        lastActionTime = VoIPController::GetCurrentTime();
        return TGVOIP_CONCTL_ACT_DECREASE;
    }
    return TGVOIP_CONCTL_ACT_NONE;
}

uint32_t CongestionControl::GetSendLossCount()
{
    return lossCount;
}
