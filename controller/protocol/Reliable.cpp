#include "../../VoIPController.h"

using namespace tgvoip;


void VoIPController::SendPacketReliably(PendingOutgoingPacket &_pkt, double retryInterval, double timeout, uint8_t tries)
{
    ENFORCE_MSG_THREAD;
#ifdef LOG_PACKETS
    LOGV("Send reliably, type=%u, len=%u, retry=%.3f, timeout=%.3f, tries=%hhu", type, unsigned(len), retryInterval, timeout, tries);
#endif
    ReliableOutgoingPacket pkt{
        std::move(_pkt),
        retryInterval,
        timeout,
        tries};
    reliablePackets.push_back(move(pkt));
    messageThread.Post(std::bind(&VoIPController::UpdateReliablePackets, this));
    if (timeout > 0.0)
    {
        messageThread.Post(std::bind(&VoIPController::UpdateReliablePackets, this), timeout);
    }
}

void VoIPController::UpdateReliablePackets()
{
    vector<PendingOutgoingPacket> packetsToSend;
    for (auto qp = reliablePackets.begin(); qp != reliablePackets.end();)
    {
        if (qp->timeout > 0 && qp->firstSentTime > 0 && GetCurrentTime() - qp->firstSentTime >= qp->timeout)
        {
#ifdef LOG_PACKETS
            LOGD("Removing reliable queued packet because of timeout");
#endif
            qp = reliablePackets.erase(qp);
            continue;
        }
        if (!qp->tries--)
        {
#ifdef LOG_PACKETS
            LOGD("Removing reliable queued packet because of no more tries");
#endif
            qp = reliablePackets.erase(qp);
            continue;
        }
        if (GetCurrentTime() - qp->lastSentTime >= qp->retryInterval)
        {
            messageThread.Post(std::bind(&VoIPController::UpdateReliablePackets, this), qp->retryInterval);
            qp->lastSentTime = GetCurrentTime();
#ifdef LOG_PACKETS
            LOGD("Sending reliable queued packet, seq=%u, len=%lu", qp->seq, qp->data->Length());
#endif
            if (qp->firstSentTime == 0)
                qp->firstSentTime = qp->lastSentTime;

            packetsToSend.push_back(qp->pkt);
        }
        ++qp;
    }
    for (auto &pkt : packetsToSend)
    {
        SendOrEnqueuePacket(pkt);
    }
}
void VoIPController::HandleReliablePackets(const PacketManager &pm)
{
    for (auto it = reliablePackets.begin(); it != reliablePackets.end();)
    {
        if (pm.wasLocalAcked(it->pkt.pktInfo.seq))
        {
            LOGV("Acked queued packet with %hhu tries left", it->tries);
            it = reliablePackets.erase(it);
            continue;
        }
        ++it;
    }

    for (auto it = currentExtras.begin(); it != currentExtras.end();)
    {
        bool acked = false;
        for (const auto &seq : it->seqs)
        {
            if (seq && pm.wasLocalAcked(seq))
            {

                LOGV("Peer acknowledged extra type %s", it->data.print().c_str());
                ProcessAcknowledgedOutgoingExtra(*it);
                it = currentExtras.erase(it);
                acked = true;
                break;
            }
        }
        if (!acked)
            ++it;
    }
}