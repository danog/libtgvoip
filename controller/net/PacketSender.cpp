#include "PacketSender.h"

using namespace tgvoip;

void PacketSender::SendExtra(Buffer &data, unsigned char type)
{
    controller->SendExtra(data, type);
}

void PacketSender::IncrementUnsentStreamPackets()
{
    controller->unsentStreamPackets++;
}

uint32_t PacketSender::SendPacket(PendingOutgoingPacket pkt)
{
    uint32_t seq = controller->nextLocalSeq();
    pkt.seq = seq;
    controller->SendOrEnqueuePacket(std::move(pkt), true, this);
    return seq;
}

double PacketSender::GetConnectionInitTime()
{
    return controller->connectionInitTime;
}

const HistoricBuffer<double, 32> &PacketSender::RTTHistory() const
{
    return controller->rttHistory;
}

MessageThread &PacketSender::GetMessageThread()
{
    return controller->messageThread;
}

const VoIPController::ProtocolInfo &PacketSender::GetProtocolInfo() const
{
    return controller->protocolInfo;
}

void PacketSender::SendStreamFlags(VoIPController::Stream &stm)
{
    controller->SendStreamFlags(stm);
}

const VoIPController::Config &PacketSender::GetConfig() const
{
    return controller->config;
}