#include "../../VoIPController.h"

using namespace tgvoip;
using namespace std;



void VoIPController::SendRelayPings()
{
    ENFORCE_MSG_THREAD;

    if ((state == STATE_ESTABLISHED || state == STATE_RECONNECTING) && endpoints.size() > 1)
    {
        Endpoint *_preferredRelay = &endpoints.at(preferredRelay);
        Endpoint *_currentEndpoint = &endpoints.at(currentEndpoint);
        Endpoint *minPingRelay = _preferredRelay;
        double minPing = _preferredRelay->averageRTT * (_preferredRelay->type == Endpoint::Type::TCP_RELAY ? 2 : 1);
        if (minPing == 0.0) // force the switch to an available relay, if any
            minPing = DBL_MAX;
        for (pair<const int64_t, Endpoint> &_endpoint : endpoints)
        {
            Endpoint &endpoint = _endpoint.second;
            if (endpoint.type == Endpoint::Type::TCP_RELAY && !useTCP)
                continue;
            if (endpoint.type == Endpoint::Type::UDP_RELAY && !useUDP)
                continue;
            if (GetCurrentTime() - endpoint.lastPingTime >= 10)
            {
                LOGV("Sending ping to %s", endpoint.GetAddress().ToString().c_str());
                SendOrEnqueuePacket(PendingOutgoingPacket{
                    /*.seq=*/(endpoint.lastPingSeq = GenerateOutSeq()),
                    /*.type=*/PKT_PING,
                    /*.len=*/0,
                    /*.data=*/Buffer(),
                    /*.endpoint=*/endpoint.id});
                endpoint.lastPingTime = GetCurrentTime();
            }
            if ((useUDP && endpoint.type == Endpoint::Type::UDP_RELAY) || (useTCP && endpoint.type == Endpoint::Type::TCP_RELAY))
            {
                double k = endpoint.type == Endpoint::Type::UDP_RELAY ? 1 : 2;
                if (endpoint.averageRTT > 0 && endpoint.averageRTT * k < minPing * relaySwitchThreshold)
                {
                    minPing = endpoint.averageRTT * k;
                    minPingRelay = &endpoint;
                }
            }
        }
        if (minPingRelay->id != preferredRelay)
        {
            preferredRelay = minPingRelay->id;
            _preferredRelay = minPingRelay;
            LOGV("set preferred relay to %s", _preferredRelay->address.ToString().c_str());
            if (_currentEndpoint->IsReflector())
            {
                currentEndpoint = preferredRelay;
                _currentEndpoint = _preferredRelay;
            }
        }
        if (_currentEndpoint->type == Endpoint::Type::UDP_RELAY && useUDP)
        {
            constexpr int64_t p2pID = static_cast<int64_t>(FOURCC('P', '2', 'P', '4')) << 32;
            constexpr int64_t lanID = static_cast<int64_t>(FOURCC('L', 'A', 'N', '4')) << 32;

            if (endpoints.find(p2pID) != endpoints.end())
            {
                Endpoint &p2p = endpoints[p2pID];
                if (endpoints.find(lanID) != endpoints.end() && endpoints[lanID].averageRTT > 0 && endpoints[lanID].averageRTT < minPing * relayToP2pSwitchThreshold)
                {
                    currentEndpoint = lanID;
                    LOGI("Switching to p2p (LAN)");
                }
                else
                {
                    if (p2p.averageRTT > 0 && p2p.averageRTT < minPing * relayToP2pSwitchThreshold)
                    {
                        currentEndpoint = p2pID;
                        LOGI("Switching to p2p (Inet)");
                    }
                }
            }
        }
        else
        {
            if (minPing > 0 && minPing < _currentEndpoint->averageRTT * p2pToRelaySwitchThreshold)
            {
                LOGI("Switching to relay");
                currentEndpoint = preferredRelay;
            }
        }
    }
}


void VoIPController::UpdateQueuedPackets()
{
    vector<PendingOutgoingPacket> packetsToSend;
    for (std::vector<QueuedPacket>::iterator qp = queuedPackets.begin(); qp != queuedPackets.end();)
    {
        if (qp->timeout > 0 && qp->firstSentTime > 0 && GetCurrentTime() - qp->firstSentTime >= qp->timeout)
        {
            LOGD("Removing queued packet because of timeout");
            qp = queuedPackets.erase(qp);
            continue;
        }
        if (GetCurrentTime() - qp->lastSentTime >= qp->retryInterval)
        {
            messageThread.Post(std::bind(&VoIPController::UpdateQueuedPackets, this), qp->retryInterval);
            uint32_t seq = GenerateOutSeq();
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

void VoIPController::SendNopPacket()
{
    if (state != STATE_ESTABLISHED)
        return;
    SendOrEnqueuePacket(PendingOutgoingPacket{
        /*.seq=*/(firstSentPing = GenerateOutSeq()),
        /*.type=*/PKT_NOP,
        /*.len=*/0,
        /*.data=*/Buffer(),
        /*.endpoint=*/0});
}

void VoIPController::SendPublicEndpointsRequest()
{
    ENFORCE_MSG_THREAD;
    if (!allowP2p)
        return;
    LOGI("Sending public endpoints request");
    for (pair<const int64_t, Endpoint> &e : endpoints)
    {
        if (e.second.type == Endpoint::Type::UDP_RELAY && !e.second.IsIPv6Only())
        {
            SendPublicEndpointsRequest(e.second);
        }
    }
    publicEndpointsReqCount++;
    if (publicEndpointsReqCount < 10)
    {
        messageThread.Post(
            [this] {
                if (waitingForRelayPeerInfo)
                {
                    LOGW("Resending peer relay info request");
                    SendPublicEndpointsRequest();
                }
            },
            5.0);
    }
    else
    {
        publicEndpointsReqCount = 0;
    }
}



void VoIPController::SendPublicEndpointsRequest(const Endpoint &relay)
{
    if (!useUDP)
        return;
    LOGD("Sending public endpoints request to %s:%d", relay.address.ToString().c_str(), relay.port);
    publicEndpointsReqTime = GetCurrentTime();
    waitingForRelayPeerInfo = true;
    Buffer buf(32);
    memcpy(*buf, relay.peerTag, 16);
    memset(*buf + 16, 0xFF, 16);
    udpSocket->Send(NetworkPacket{
        std::move(buf),
        relay.address,
        relay.port,
        NetworkProtocol::UDP});
}

Endpoint &VoIPController::GetEndpointByType(const Endpoint::Type type)
{
    if (type == Endpoint::Type::UDP_RELAY && preferredRelay)
        return endpoints.at(preferredRelay);
    for (pair<const int64_t, Endpoint> &e : endpoints)
    {
        if (e.second.type == type)
            return e.second;
    }
    throw out_of_range("no endpoint");
}

void VoIPController::SendPacketReliably(unsigned char type, unsigned char *data, size_t len, double retryInterval, double timeout)
{
    ENFORCE_MSG_THREAD;

    LOGD("Send reliably, type=%u, len=%u, retry=%.3f, timeout=%.3f", type, unsigned(len), retryInterval, timeout);
    QueuedPacket pkt;
    if (data)
    {
        Buffer b(len);
        b.CopyFrom(data, 0, len);
        pkt.data = move(b);
    }
    pkt.type = type;
    pkt.retryInterval = retryInterval;
    pkt.timeout = timeout;
    pkt.firstSentTime = 0;
    pkt.lastSentTime = 0;
    queuedPackets.push_back(move(pkt));
    messageThread.Post(std::bind(&VoIPController::UpdateQueuedPackets, this));
    if (timeout > 0.0)
    {
        messageThread.Post(std::bind(&VoIPController::UpdateQueuedPackets, this), timeout);
    }
}

void VoIPController::SendExtra(Buffer &data, unsigned char type)
{
    ENFORCE_MSG_THREAD;

    LOGV("Sending extra type %u length %u", type, (unsigned int)data.Length());
    for (vector<UnacknowledgedExtraData>::iterator x = currentExtras.begin(); x != currentExtras.end(); ++x)
    {
        if (x->type == type)
        {
            x->firstContainingSeq = 0;
            x->data = move(data);
            return;
        }
    }
    UnacknowledgedExtraData xd = {type, move(data), 0};
    currentExtras.push_back(move(xd));
}

void VoIPController::SendUdpPing(Endpoint &endpoint)
{
    if (endpoint.type != Endpoint::Type::UDP_RELAY)
        return;
    BufferOutputStream p(1024);
    p.WriteBytes(endpoint.peerTag, 16);
    p.WriteInt32(-1);
    p.WriteInt32(-1);
    p.WriteInt32(-1);
    p.WriteInt32(-2);
    int64_t id;
    crypto.rand_bytes(reinterpret_cast<uint8_t *>(&id), 8);
    p.WriteInt64(id);
    endpoint.udpPingTimes[id] = GetCurrentTime();
    udpSocket->Send(NetworkPacket{
        Buffer(std::move(p)),
        endpoint.GetAddress(),
        endpoint.port,
        NetworkProtocol::UDP});
    endpoint.totalUdpPings++;
    LOGV("Sending UDP ping to %s:%d, id %" PRId64, endpoint.GetAddress().ToString().c_str(), endpoint.port, id);
}

void VoIPController::ResetUdpAvailability()
{
    ENFORCE_MSG_THREAD;

    LOGI("Resetting UDP availability");
    if (udpPingTimeoutID != MessageThread::INVALID_ID)
    {
        messageThread.Cancel(udpPingTimeoutID);
    }
    {
        for (pair<const int64_t, Endpoint> &e : endpoints)
        {
            e.second.udpPongCount = 0;
            e.second.udpPingTimes.clear();
        }
    }
    udpPingCount = 0;
    udpConnectivityState = UDP_PING_PENDING;
    udpPingTimeoutID = messageThread.Post(std::bind(&VoIPController::SendUdpPings, this), 0.0, 0.5);
}

void VoIPController::ResetEndpointPingStats()
{
    ENFORCE_MSG_THREAD;

    for (pair<const int64_t, Endpoint> &e : endpoints)
    {
        e.second.averageRTT = 0.0;
        e.second.rtts.Reset();
    }
}


void VoIPController::SendStreamFlags(Stream &stream)
{
    ENFORCE_MSG_THREAD;

    BufferOutputStream s(5);
    s.WriteByte(stream.id);
    uint32_t flags = 0;
    if (stream.enabled)
        flags |= STREAM_FLAG_ENABLED;
    if (stream.extraECEnabled)
        flags |= STREAM_FLAG_EXTRA_EC;
    if (stream.paused)
        flags |= STREAM_FLAG_PAUSED;
    s.WriteInt32(flags);
    LOGV("My stream state: id %u flags %u", (unsigned int)stream.id, (unsigned int)flags);
    Buffer buf(move(s));
    SendExtra(buf, EXTRA_TYPE_STREAM_FLAGS);
}