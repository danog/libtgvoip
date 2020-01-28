#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

void VoIPController::SendPacketReliably(unsigned char type, unsigned char *data, size_t len, double retryInterval, double timeout, uint8_t tries)
{
    ENFORCE_MSG_THREAD;

    LOGV("Send reliably, type=%u, len=%u, retry=%.3f, timeout=%.3f, tries=%hhu", type, unsigned(len), retryInterval, timeout, tries);
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
            LOGD("Removing queued packet because of timeout");
            qp = reliablePackets.erase(qp);
            continue;
        }
        if (!qp->tries--)
        {
            LOGD("Removing queued packet because of no more tries");
            qp = reliablePackets.erase(qp);
            continue;
        }
        if (GetCurrentTime() - qp->lastSentTime >= qp->retryInterval)
        {
            messageThread.Post(std::bind(&VoIPController::UpdateReliablePackets, this), qp->retryInterval);
            uint32_t seq = packetManager.nextLocalSeq();
            qp->seqs.Add(seq);
            qp->lastSentTime = GetCurrentTime();
            //LOGD("Sending queued packet, seq=%u, type=%u, len=%u", seq, qp.type, qp.data.Length());
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
            if (!qp.seqs[i] || (didAck = WasOutgoingPacketAcknowledged(qp.seqs[i], false)))
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

bool VoIPController::WasOutgoingPacketAcknowledged(uint32_t seq, bool checkAll)
{
    bool res = getBestPacketManager().wasLocalAcked(seq);
    if (res || !checkAll)
    {
        return res;
    }

    RecentOutgoingPacket *pkt = GetRecentOutgoingPacket(seq);
    if (!pkt)
        return false;
    return pkt->ackTime != 0.0;
}