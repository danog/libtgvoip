#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

void VoIPController::SendPacketReliably(unsigned char type, unsigned char *data, size_t len, double retryInterval, double timeout, uint8_t tries)
{
    ENFORCE_MSG_THREAD;
#ifdef LOG_PACKETS
    LOGV("Send reliably, type=%u, len=%u, retry=%.3f, timeout=%.3f, tries=%hhu", type, unsigned(len), retryInterval, timeout, tries);
#endif
    ReliableOutgoingPacket pkt;
    if (data)
    {
        Buffer b(len);
        b.CopyFrom(data, 0, len);
        pkt.data = move(b);
    }
    pkt.type = type;
    pkt.retryInterval = retryInterval;
    pkt.timeout = timeout;
    pkt.tries = tries;
    pkt.firstSentTime = 0;
    pkt.lastSentTime = 0;
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
    for (std::vector<ReliableOutgoingPacket>::iterator qp = reliablePackets.begin(); qp != reliablePackets.end();)
    {
        if (qp->timeout > 0 && qp->firstSentTime > 0 && GetCurrentTime() - qp->firstSentTime >= qp->timeout)
        {
//#ifdef LOG_PACKETS
            LOGD("Removing queued packet because of timeout");
//#endif
            qp = reliablePackets.erase(qp);
            continue;
        }
        if (!qp->tries--)
        {
//#ifdef LOG_PACKETS
            LOGD("Removing queued packet because of no more tries");
//#endif
            qp = reliablePackets.erase(qp);
            continue;
        }
        if (GetCurrentTime() - qp->lastSentTime >= qp->retryInterval)
        {
            messageThread.Post(std::bind(&VoIPController::UpdateReliablePackets, this), qp->retryInterval);
            uint32_t seq = packetManager.nextLocalSeq();
            qp->seqs.Add(seq);
            qp->lastSentTime = GetCurrentTime();
//#ifdef LOG_PACKETS
            LOGD("Sending reliable queued packet, seq=%u, type=%u, len=%lu", seq, qp->type, qp->data.Length());
//#endif
            Buffer buf(qp->data.Length());
            if (qp->firstSentTime == 0)
                qp->firstSentTime = qp->lastSentTime;
            if (qp->data.Length())
                buf.CopyFrom(qp->data, qp->data.Length());
            packetsToSend.push_back(PendingOutgoingPacket{
                /*.seq=*/seq,
                /*.type=*/qp->type,
                /*.len=*/qp->data.Length(),
                /*.data=*/move(buf),
                /*.endpoint=*/0});
        }
        ++qp;
    }
    for (PendingOutgoingPacket &pkt : packetsToSend)
    {
        SendOrEnqueuePacket(move(pkt));
    }
}
void VoIPController::handleReliablePackets()
{
    for (auto it = reliablePackets.begin(); it != reliablePackets.end();)
    {
        ReliableOutgoingPacket &qp = *it;
        bool didAck = false;
        for (uint8_t i = 0; i < qp.seqs.Size(); ++i)
        {
            if (!qp.seqs[i] || (didAck = packetManager.wasLocalAcked(qp.seqs[i])))
                break;
        }
        if (didAck)
        {
            LOGV("Acked queued packet with %hhu tries left", qp.tries);
            it = reliablePackets.erase(it);
            continue;
        }
        ++it;
    }
}