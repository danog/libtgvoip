//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "CongestionControl.h"
#include "VoIPController.h"
#include "tools/logging.h"
#include "VoIPServerConfig.h"
#include "../PrivateDefines.h"
#include <math.h>
#include <assert.h>

using namespace tgvoip;

CongestionControl::CongestionControl() : cwnd(static_cast<size_t>(ServerConfig::GetSharedInstance()->GetInt("audio_congestion_window", 1024)))
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

void CongestionControl::PacketAcknowledged(uint32_t seq)
{
	for (auto &packet : inflightPackets)
	{
		if (packet.seq == seq && packet.sendTime > 0)
		{
			tmpRtt += (VoIPController::GetCurrentTime() - packet.sendTime);
			tmpRttCount++;
			packet.sendTime = 0;
			inflightDataSize -= packet.size;
			break;
		}
	}
}

void CongestionControl::PacketSent(uint32_t seq, size_t size)
{
	if (!seqgt(seq, lastSentSeq) || seq == lastSentSeq)
	{
		LOGW("Duplicate outgoing seq %u", seq);
		return;
	}
	lastSentSeq = seq;
	double smallestSendTime = INFINITY;
	tgvoip_congestionctl_packet_t *slot = NULL;
	for (size_t i = 0; i < inflightPackets.size(); i++)
	{
		if (inflightPackets[i].sendTime == 0)
		{
			slot = &inflightPackets[i];
			break;
		}
		if (smallestSendTime > inflightPackets[i].sendTime)
		{
			slot = &inflightPackets[i];
			smallestSendTime = slot->sendTime;
		}
	}
	assert(slot != NULL);
	if (slot->sendTime > 0)
	{
		inflightDataSize -= slot->size;
		lossCount++;
		LOGD("Packet with seq %u was not acknowledged", slot->seq);
	}
	slot->seq = seq;
	slot->size = size;
	slot->sendTime = VoIPController::GetCurrentTime();
	inflightDataSize += size;
}

void CongestionControl::PacketLost(uint32_t seq)
{
	for (auto &packet : inflightPackets)
	{
		if (packet.seq == seq && packet.sendTime > 0)
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
		if (packet.sendTime != 0 && VoIPController::GetCurrentTime() - packet.sendTime > TGVOIP_CONCTL_LOST_AFTER)
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
	size_t max = cwnd + cwnd / 10;
	size_t min = cwnd - cwnd / 10;
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
