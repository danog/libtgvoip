//
// Created by Grishka on 19/03/2019.
//

#ifndef LIBTGVOIP_PACKETSENDER_H
#define LIBTGVOIP_PACKETSENDER_H

#include "../../VoIPController.h"
#include "../protocol/PacketStructs.h"
#include "../protocol/PacketManager.h"
#include <functional>
#include <stdint.h>

namespace tgvoip
{
class PacketSender
{
public:
	PacketSender(VoIPController *controller, const std::shared_ptr<VoIPController::Stream> &stream) : controller(controller), stream(stream), packetManager(stream->id - 1){};
	virtual ~PacketSender() = default;
	virtual void PacketAcknowledged(uint32_t seq, double sendTime, double ackTime, uint8_t type, uint32_t size) = 0;
	virtual void PacketLost(uint32_t seq, uint8_t type, uint32_t size) = 0;

	inline PacketManager &getPacketManager()
	{
		return packetManager;
	}
protected:
	inline void SendExtra(Buffer &data, unsigned char type)
	{
		controller->SendExtra(data, type);
	}

	inline void IncrementUnsentStreamPackets()
	{
		controller->unsentStreamPackets++;
	}

	inline uint32_t SendPacket(PendingOutgoingPacket pkt)
	{
		uint32_t seq = controller->peerVersion < PROTOCOL_RELIABLE ? controller->packetManager.nextLocalSeq() : packetManager.nextLocalSeq();
		pkt.seq = seq;
		controller->SendOrEnqueuePacket(std::move(pkt), true, this);
		return seq;
	}

	inline void SendPacketReliably(unsigned char type, unsigned char *data, size_t len, double retryInterval, double timeout, uint8_t tries = 0xFF)
	{
		controller->SendPacketReliably(type, data, len, retryInterval, timeout, tries);
	}

	inline double GetConnectionInitTime()
	{
		return controller->connectionInitTime;
	}

	inline const HistoricBuffer<double, 32> &RTTHistory() const
	{
		return controller->rttHistory;
	}

	inline MessageThread &GetMessageThread()
	{
		return controller->messageThread;
	}

	inline const VoIPController::ProtocolInfo &GetProtocolInfo() const
	{
		return controller->protocolInfo;
	}

	inline void SendStreamFlags(VoIPController::Stream &stm)
	{
		controller->SendStreamFlags(stm);
	}

	inline const VoIPController::Config &GetConfig() const
	{
		return controller->config;
	}
	inline const bool IsStopping() const
	{
		return controller->stopping;
	}
	inline const bool ReceivedInitAck() const
	{
		return controller->receivedInitAck;
	}

	inline const int32_t PeerVersion() const
	{
		return controller->peerVersion;
	}

	inline const double LastRtt() const
	{
		return controller->rttHistory[0];
	}

	VoIPController *controller;

	std::shared_ptr<VoIPController::Stream> stream;

	PacketManager packetManager;

	std::vector<PendingOutgoingPacket> reliableQueue;
};
} // namespace tgvoip

#endif //LIBTGVOIP_PACKETSENDER_H
