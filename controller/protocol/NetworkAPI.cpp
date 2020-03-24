#include "../PrivateDefines.cpp"
//#include <random>

using namespace tgvoip;
using namespace std;

//std::random_device dev;
//std::mt19937 rng(dev());
//std::uniform_int_distribution<std::mt19937::result_type> dist6(0, 9); // distribution in range [1, 6]

PendingOutgoingPacket VoIPController::PreparePacket(unsigned char *data, size_t len, Endpoint &ep, CongestionControlPacket &&pkt)
{
    BufferOutputStream out(len + 128);
    if (ep.IsReflector())
        out.WriteBytes((unsigned char *)ep.peerTag, 16);
    else if (ver.peerVersion < 9)
        out.WriteBytes(callID, 16);

    if (len > 0)
    {
        encryptPacket(data, len, out);
    }

    return PendingOutgoingPacket(std::make_shared<Buffer>(std::move(out)), std::move(pkt), ep.id);
}
void VoIPController::SendPacket(OutgoingPacket &&pkt, double retryInterval, double timeout, uint8_t tries)
{
    ENFORCE_MSG_THREAD;
    bool isReliable = tries;
    Endpoint &endpoint = *GetEndpointForPacket(pkt);
    Packet &packet = pkt.packet;
    PacketManager &pm = outgoingStreams[packet.streamId]->packetManager;

    if (ver.isNew())
    {
        packet.prepare(pm, currentExtras, endpoint.id);

        //BufferOutputStream out(packet.getSize()); // Can precalc, should check if it's worth it
        BufferOutputStream out(1500);
        packet.serialize(out, ver);

        auto res = PreparePacket(out.GetBuffer(), out.GetLength(), endpoint, CongestionControlPacket(packet));
        if (isReliable)
        {
            SendPacketReliably(res, retryInterval, timeout, tries);
        }
        else
        {
            SendOrEnqueuePacket(res);
        }
    }
    else
    {
        PacketManager &legacyPm = outgoingStreams[StreamId::Signaling]->packetManager;
        packet.prepare(pm, currentExtras, endpoint.id, legacyPm, ver.peerVersion);

        uint32_t seq = legacyPm.getLocalSeq();

        std::vector<std::tuple<unsigned char *, size_t, bool>> out;
        packet.serializeLegacy(out, ver, state, callID);
        legacyPm.setLocalSeq(packet.legacySeq);

        size_t length = 0;
        for (auto &t : out)
        {
            auto res = PreparePacket(std::get<0>(t), std::get<1>(t), endpoint, CongestionControlPacket(seq++, StreamId::Signaling));
            if (std::get<2>(t) || isReliable)
            {
                SendPacketReliably(res, retryInterval, timeout, tries);
            }
            else
            {
                SendOrEnqueuePacket(res);
            }
        }
    }
}

bool VoIPController::SendOrEnqueuePacket(PendingOutgoingPacket &pkt, bool enqueue)
{
    Endpoint &endpoint = *GetEndpointForPacket(pkt);
    bool canSend;
    if (endpoint.type != Endpoint::Type::TCP_RELAY)
    {
        canSend = realUdpSocket->IsReadyToSend();
    }
    else
    {
        if (!endpoint.socket)
        {
            LOGV("Connecting to %s:%u", endpoint.GetAddress().ToString().c_str(), endpoint.port);
            if (proxyProtocol == PROXY_NONE)
            {
                endpoint.socket = make_shared<NetworkSocketTCPObfuscated>(NetworkSocket::Create(NetworkProtocol::TCP));
                endpoint.socket->Connect(endpoint.GetAddress(), endpoint.port);
            }
            else if (proxyProtocol == PROXY_SOCKS5)
            {
                std::shared_ptr<NetworkSocket> tcp = NetworkSocket::Create(NetworkProtocol::TCP);
                tcp->Connect(resolvedProxyAddress, proxyPort);
                shared_ptr<NetworkSocketSOCKS5Proxy> proxy = make_shared<NetworkSocketSOCKS5Proxy>(tcp, nullptr, proxyUsername, proxyPassword);
                endpoint.socket = proxy;
                endpoint.socket->Connect(endpoint.GetAddress(), endpoint.port);
            }
            selectCanceller->CancelSelect();
        }
        canSend = endpoint.socket && endpoint.socket->IsReadyToSend();
    }
    if (!canSend)
    {
        if (enqueue)
        {
            LOGW("Not ready to send - enqueueing");
            sendQueue.push_back(move(pkt));
        }
        return false;
    }
    if ((endpoint.type == Endpoint::Type::TCP_RELAY && useTCP) || (endpoint.type != Endpoint::Type::TCP_RELAY && useUDP))
    {
        if (stopping)
            return;
        if (endpoint.type == Endpoint::Type::TCP_RELAY && !useTCP)
            return;

        conctl.PacketSent(pkt.pktInfo, pkt.packet->Length());

        rawSendQueue.Put(
            RawPendingOutgoingPacket{
                NetworkPacket{
                    pkt.packet,
                    endpoint.GetAddress(),
                    endpoint.port,
                    endpoint.type == Endpoint::Type::TCP_RELAY ? NetworkProtocol::TCP : NetworkProtocol::UDP},
                endpoint.type == Endpoint::Type::TCP_RELAY ? endpoint.socket : nullptr});

        unacknowledgedIncomingPacketCount = 0;
        outgoingStreams[pkt.pktInfo.streamId]->packetManager.addRecentOutgoingPacket(pkt);
//LOGV("Sending %d bytes to %s:%d", out.GetLength(), ep.address.ToString().c_str(), ep.port);
#ifdef LOG_PACKETS
        //LOGV("Sending: to=%s:%u, seq=%u, length=%u, type=%s, streamId=%hhu", ep.GetAddress().ToString().c_str(), ep.port, seq, (unsigned int)out.GetLength(), GetPacketTypeString(type).c_str(), streamId);
#endif
    }
    return true;
}

void VoIPController::SendInit()
{
    ENFORCE_MSG_THREAD;

    auto init = std::make_shared<ExtraInit>();
    init->peerVersion = PROTOCOL_VERSION;
    init->minVersion = MIN_PROTOCOL_VERSION;
    if (config.enableCallUpgrade)
        init->flags |= ExtraInit::Flags::GroupCallSupported;
    if (config.enableVideoReceive)
        init->flags |= ExtraInit::Flags::VideoRecvSupported;
    if (config.enableVideoSend)
        init->flags |= ExtraInit::Flags::VideoSendSupported;
    if (dataSavingMode)
        init->flags |= ExtraInit::Flags::DataSavingEnabled;

    init->audioCodecs.v.push_back(Codec::Opus);
    if (config.enableVideoReceive)
    {
        for (auto &decoder : video::VideoRenderer::GetAvailableDecoders())
        {
            init->decoders.v.push_back(decoder);
        }
    }
    init->maxResolution = ver.connectionMaxLayer >= 92 ? video::VideoRenderer::GetMaximumResolution() : 0;

    SendExtra(init);

    auto &pm = outgoingStreams[StreamId::Signaling]->packetManager;
    uint32_t seq = pm.nextLocalSeq();

    for (pair<const int64_t, Endpoint> &_e : endpoints)
    {
        Endpoint &e = _e.second;
        if (e.type == Endpoint::Type::TCP_RELAY && !useTCP)
            continue;
        Packet p;
        p.seq = seq;
        p.ackSeq = pm.getLastRemoteSeq();
        p.ackMask = pm.getRemoteAckMask();
        SendPacket(OutgoingPacket(std::move(p), e.id));
    }

    if (state == STATE_WAIT_INIT)
        SetState(STATE_WAIT_INIT_ACK);

    messageThread.Post(
        [this] {
            if (state == STATE_WAIT_INIT_ACK)
            {
                SendInit();
            }
        },
        0.5);
}

void VoIPController::InitUDPProxy()
{
    if (realUdpSocket != udpSocket)
    {
        udpSocket->Close();
        udpSocket = realUdpSocket;
    }
    char sbuf[128];
    snprintf(sbuf, sizeof(sbuf), "%s:%u", proxyAddress.c_str(), proxyPort);
    string proxyHostPort(sbuf);
    if (proxyHostPort == lastTestedProxyServer && !proxySupportsUDP)
    {
        LOGI("Proxy does not support UDP - using UDP directly instead");
        messageThread.Post(bind(&VoIPController::ResetUdpAvailability, this));
        return;
    }

    std::shared_ptr<NetworkSocket> tcp = NetworkSocket::Create(NetworkProtocol::TCP);
    tcp->Connect(resolvedProxyAddress, proxyPort);

    vector<std::shared_ptr<NetworkSocket>> writeSockets;
    vector<std::shared_ptr<NetworkSocket>> readSockets;
    vector<std::shared_ptr<NetworkSocket>> errorSockets;

    while (!tcp->IsFailed() && !tcp->IsReadyToSend())
    {
        writeSockets.push_back(tcp);
        if (!NetworkSocket::Select(readSockets, writeSockets, errorSockets, selectCanceller))
        {
            LOGW("Select canceled while waiting for proxy control socket to connect");
            return;
        }
    }
    LOGV("UDP proxy control socket ready to send");
    std::shared_ptr<NetworkSocketSOCKS5Proxy> udpProxy = std::make_shared<NetworkSocketSOCKS5Proxy>(tcp, realUdpSocket, proxyUsername, proxyPassword);
    udpProxy->OnReadyToSend();
    writeSockets.clear();
    while (!udpProxy->IsFailed() && !tcp->IsFailed() && !udpProxy->IsReadyToSend())
    {
        readSockets.clear();
        errorSockets.clear();
        readSockets.push_back(tcp);
        errorSockets.push_back(tcp);
        if (!NetworkSocket::Select(readSockets, writeSockets, errorSockets, selectCanceller))
        {
            LOGW("Select canceled while waiting for UDP proxy to initialize");
            return;
        }
        if (!readSockets.empty())
            udpProxy->OnReadyToReceive();
    }
    LOGV("UDP proxy initialized");

    if (udpProxy->IsFailed())
    {
        udpProxy->Close();
        proxySupportsUDP = false;
    }
    else
    {
        udpSocket = udpProxy;
    }
    messageThread.Post(bind(&VoIPController::ResetUdpAvailability, this));
}

void VoIPController::TrySendOutgoingPackets()
{
    ENFORCE_MSG_THREAD;

    for (vector<PendingOutgoingPacket>::iterator opkt = sendQueue.begin(); opkt != sendQueue.end();)
    {
        Endpoint *endpoint = GetEndpointForPacket(*opkt);
        if (!endpoint)
        {
            opkt = sendQueue.erase(opkt);
            LOGE("SendQueue contained packet for nonexistent endpoint");
            continue;
        }
        bool canSend;
        if (endpoint->type != Endpoint::Type::TCP_RELAY)
            canSend = realUdpSocket->IsReadyToSend();
        else
            canSend = endpoint->socket && endpoint->socket->IsReadyToSend();
        if (canSend)
        {
            LOGI("Sending queued packet");
            SendOrEnqueuePacket(*opkt, false);
            opkt = sendQueue.erase(opkt);
        }
        else
        {
            ++opkt;
        }
    }
}

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
                SendExtra(std::make_shared<ExtraPing>(), endpoint.id);
                SendNopPacket(endpoint.id);
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
            if (endpoints.find(Endpoint::ID::P2Pv4) != endpoints.end())
            {
                Endpoint &p2p = endpoints[Endpoint::ID::P2Pv4];
                if (endpoints.find(Endpoint::ID::LANv4) != endpoints.end() && endpoints[Endpoint::ID::LANv4].averageRTT > 0 && endpoints[Endpoint::ID::LANv4].averageRTT < minPing * relayToP2pSwitchThreshold)
                {
                    currentEndpoint = Endpoint::ID::LANv4;
                    LOGI("Switching to p2p (LAN)");
                }
                else
                {
                    if (p2p.averageRTT > 0 && p2p.averageRTT < minPing * relayToP2pSwitchThreshold)
                    {
                        currentEndpoint = Endpoint::ID::P2Pv4;
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

void VoIPController::SendNopPacket(int64_t endpointId, double retryInterval, double timeout, uint8_t tries)
{
    if (state != STATE_ESTABLISHED)
        return;
    SendPacket(OutgoingPacket(Packet(), endpointId), retryInterval, timeout, tries);
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
    auto buf = std::make_shared<Buffer>(32);
    memcpy(**buf, relay.peerTag, 16);
    memset(**buf + 16, 0xFF, 16);
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

void VoIPController::SendDataSavingMode()
{
    ENFORCE_MSG_THREAD;

    auto s = std::make_shared<ExtraNetworkChanged>();
    s->flags |= dataSavingMode ? ExtraInit::Flags::DataSavingEnabled : 0;
    SendExtra(s);
}

void VoIPController::SendExtra(std::shared_ptr<Extra> &&_d, int64_t endpointId)
{
    SendExtra(Wrapped<Extra>(_d), endpointId);
}
void VoIPController::SendExtra(std::shared_ptr<Extra> &_d, int64_t endpointId)
{
    SendExtra(Wrapped<Extra>(std::move(_d)), endpointId);
}
void VoIPController::SendExtra(Wrapped<Extra> &&extra, int64_t endpointId)
{
    ENFORCE_MSG_THREAD;

    auto type = extra.getID();
    LOGV("Sending extra type %hhu", type);
    for (auto &extra : currentExtras)
    {
        if (extra.data.getID() == type && extra.endpointId == endpointId)
        {
            extra.seqs.Reset();
            extra.data = std::move(extra.data);
            return;
        }
    }
    currentExtras.push_back(UnacknowledgedExtraData(std::move(extra), endpointId));
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
        std::make_shared<Buffer>(std::move(p)),
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
