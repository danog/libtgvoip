//
// Created by Grishka on 19/03/2019.
//

#ifndef LIBTGVOIP_PACKETSENDER_H
#define LIBTGVOIP_PACKETSENDER_H

#include "../VoIPController.h"
#include "../protocol/PacketStructs.h"
#include <functional>
#include <stdint.h>

namespace tgvoip
{
class PacketSender
{
public:
	PacketSender(VoIPController *controller) : controller(controller){};
	virtual ~PacketSender() = default;
	virtual void PacketAcknowledged(uint32_t seq, double sendTime, double ackTime, uint8_t type, uint32_t size) = 0;
	virtual void PacketLost(uint32_t seq, uint8_t type, uint32_t size) = 0;

protected:
	void SendExtra(Buffer &data, unsigned char type);

	void IncrementUnsentStreamPackets();

	uint32_t SendPacket(PendingOutgoingPacket pkt);

	double GetConnectionInitTime();

	const HistoricBuffer<double, 32> &RTTHistory() const;

	MessageThread &GetMessageThread();

	const VoIPController::ProtocolInfo &GetProtocolInfo() const;

	void SendStreamFlags(VoIPController::Stream &stm);

	const VoIPController::Config &GetConfig() const;

	VoIPController *controller;
};
} // namespace tgvoip

#endif //LIBTGVOIP_PACKETSENDER_H
