#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;


bool VoIPController::SendOrEnqueuePacket(PendingOutgoingPacket pkt, bool enqueue, PacketSender *source)
{
    ENFORCE_MSG_THREAD;

    Endpoint *endpoint = GetEndpointForPacket(pkt);
    if (!endpoint)
    {
        abort();
        return false;
    }

    bool canSend;
    if (endpoint->type != Endpoint::Type::TCP_RELAY)
    {
        canSend = realUdpSocket->IsReadyToSend();
    }
    else
    {
        if (!endpoint->socket)
        {
            LOGV("Connecting to %s:%u", endpoint->GetAddress().ToString().c_str(), endpoint->port);
            if (proxyProtocol == PROXY_NONE)
            {
                endpoint->socket = make_shared<NetworkSocketTCPObfuscated>(NetworkSocket::Create(NetworkProtocol::TCP));
                endpoint->socket->Connect(endpoint->GetAddress(), endpoint->port);
            }
            else if (proxyProtocol == PROXY_SOCKS5)
            {
                std::shared_ptr<NetworkSocket> tcp = NetworkSocket::Create(NetworkProtocol::TCP);
                tcp->Connect(resolvedProxyAddress, proxyPort);
                shared_ptr<NetworkSocketSOCKS5Proxy> proxy = make_shared<NetworkSocketSOCKS5Proxy>(tcp, nullptr, proxyUsername, proxyPassword);
                endpoint->socket = proxy;
                endpoint->socket->Connect(endpoint->GetAddress(), endpoint->port);
            }
            selectCanceller->CancelSelect();
        }
        canSend = endpoint->socket && endpoint->socket->IsReadyToSend();
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
    if ((endpoint->type == Endpoint::Type::TCP_RELAY && useTCP) || (endpoint->type != Endpoint::Type::TCP_RELAY && useUDP))
    {
        //BufferOutputStream p(buf, sizeof(buf));
        BufferOutputStream p(1500);
        WritePacketHeader(pkt.seq, &p, pkt.type, (uint32_t)pkt.len, source);
        p.WriteBytes(pkt.data);
        SendPacket(p.GetBuffer(), p.GetLength(), *endpoint, pkt);
        if (pkt.type == PKT_STREAM_DATA)
        {
            unsentStreamPackets--;
        }
    }
    return true;
}

void VoIPController::SendPacket(unsigned char *data, size_t len, Endpoint &ep, PendingOutgoingPacket &srcPacket)
{
    if (stopping)
        return;
    if (ep.type == Endpoint::Type::TCP_RELAY && !useTCP)
        return;
    BufferOutputStream out(len + 128);
    if (ep.IsReflector())
        out.WriteBytes((unsigned char *)ep.peerTag, 16);
    else if (peerVersion < 9)
        out.WriteBytes(callID, 16);
    if (len > 0)
    {
        if (useMTProto2)
        {
            BufferOutputStream inner(len + 128);
            size_t sizeSize;
            if (peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92))
            {
                inner.WriteInt16((uint16_t)len);
                sizeSize = 0;
            }
            else
            {
                inner.WriteInt32((uint32_t)len);
                out.WriteBytes(keyFingerprint, 8);
                sizeSize = 4;
            }
            inner.WriteBytes(data, len);

            size_t padLen = 16 - inner.GetLength() % 16;
            if (padLen < 16)
                padLen += 16;
            unsigned char padding[32];
            crypto.rand_bytes((uint8_t *)padding, padLen);
            inner.WriteBytes(padding, padLen);
            assert(inner.GetLength() % 16 == 0);

            unsigned char key[32], iv[32], msgKey[16];
            BufferOutputStream buf(len + 32);
            size_t x = isOutgoing ? 0 : 8;
            buf.WriteBytes(encryptionKey + 88 + x, 32);
            buf.WriteBytes(inner.GetBuffer() + sizeSize, inner.GetLength() - sizeSize);
            unsigned char msgKeyLarge[32];
            crypto.sha256(buf.GetBuffer(), buf.GetLength(), msgKeyLarge);
            memcpy(msgKey, msgKeyLarge + 8, 16);
            KDF2(msgKey, isOutgoing ? 0 : 8, key, iv);
            out.WriteBytes(msgKey, 16);
            //LOGV("<- MSG KEY: %08x %08x %08x %08x, hashed %u", *reinterpret_cast<int32_t*>(msgKey), *reinterpret_cast<int32_t*>(msgKey+4), *reinterpret_cast<int32_t*>(msgKey+8), *reinterpret_cast<int32_t*>(msgKey+12), inner.GetLength()-4);

            unsigned char aesOut[MSC_STACK_FALLBACK(inner.GetLength(), 1500)];
            crypto.aes_ige_encrypt(inner.GetBuffer(), aesOut, inner.GetLength(), key, iv);
            out.WriteBytes(aesOut, inner.GetLength());
        }
        else
        {
            BufferOutputStream inner(len + 128);
            inner.WriteInt32(static_cast<int32_t>(len));
            inner.WriteBytes(data, len);
            if (inner.GetLength() % 16 != 0)
            {
                size_t padLen = 16 - inner.GetLength() % 16;
                unsigned char padding[16];
                crypto.rand_bytes((uint8_t *)padding, padLen);
                inner.WriteBytes(padding, padLen);
            }
            assert(inner.GetLength() % 16 == 0);
            unsigned char key[32], iv[32], msgHash[SHA1_LENGTH];
            crypto.sha1((uint8_t *)inner.GetBuffer(), len + 4, msgHash);
            out.WriteBytes(keyFingerprint, 8);
            out.WriteBytes((msgHash + (SHA1_LENGTH - 16)), 16);
            KDF(msgHash + (SHA1_LENGTH - 16), isOutgoing ? 0 : 8, key, iv);
            unsigned char aesOut[MSC_STACK_FALLBACK(inner.GetLength(), 1500)];
            crypto.aes_ige_encrypt(inner.GetBuffer(), aesOut, inner.GetLength(), key, iv);
            out.WriteBytes(aesOut, inner.GetLength());
        }
    }
    //LOGV("Sending %d bytes to %s:%d", out.GetLength(), ep.address.ToString().c_str(), ep.port);
#ifdef LOG_PACKETS
    LOGV("Sending: to=%s:%u, seq=%u, length=%u, type=%s", ep.GetAddress().ToString().c_str(), ep.port, srcPacket.seq, (unsigned int)out.GetLength(), GetPacketTypeString(srcPacket.type).c_str());
#endif

    rawSendQueue.Put(
        RawPendingOutgoingPacket{
            NetworkPacket{
                Buffer(std::move(out)),
                ep.GetAddress(),
                ep.port,
                ep.type == Endpoint::Type::TCP_RELAY ? NetworkProtocol::TCP : NetworkProtocol::UDP},
            ep.type == Endpoint::Type::TCP_RELAY ? ep.socket : nullptr});
}

void VoIPController::ActuallySendPacket(NetworkPacket pkt, Endpoint &ep)
{
    //LOGI("Sending packet of %d bytes", pkt.length);
    if (IS_MOBILE_NETWORK(networkType))
        stats.bytesSentMobile += (uint64_t)pkt.data.Length();
    else
        stats.bytesSentWifi += (uint64_t)pkt.data.Length();
    if (ep.type == Endpoint::Type::TCP_RELAY)
    {
        if (ep.socket && !ep.socket->IsFailed())
        {
            ep.socket->Send(std::move(pkt));
        }
    }
    else
    {
        udpSocket->Send(std::move(pkt));
    }
}


void VoIPController::SendInit()
{
    ENFORCE_MSG_THREAD;

    uint32_t initSeq = GenerateOutSeq();
    for (pair<const int64_t, Endpoint> &_e : endpoints)
    {
        Endpoint &e = _e.second;
        if (e.type == Endpoint::Type::TCP_RELAY && !useTCP)
            continue;
        BufferOutputStream out(1024);
        out.WriteInt32(PROTOCOL_VERSION);
        out.WriteInt32(MIN_PROTOCOL_VERSION);
        uint32_t flags = 0;
        if (config.enableCallUpgrade)
            flags |= INIT_FLAG_GROUP_CALLS_SUPPORTED;
        if (config.enableVideoReceive)
            flags |= INIT_FLAG_VIDEO_RECV_SUPPORTED;
        if (config.enableVideoSend)
            flags |= INIT_FLAG_VIDEO_SEND_SUPPORTED;
        if (dataSavingMode)
            flags |= INIT_FLAG_DATA_SAVING_ENABLED;
        out.WriteInt32(flags);
        if (connectionMaxLayer < 74)
        {
            out.WriteByte(2); // audio codecs count
            out.WriteByte(CODEC_OPUS_OLD);
            out.WriteByte(0);
            out.WriteByte(0);
            out.WriteByte(0);
            out.WriteInt32(CODEC_OPUS);
            out.WriteByte(0); // video codecs count (decode)
            out.WriteByte(0); // video codecs count (encode)
        }
        else
        {
            out.WriteByte(1);
            out.WriteInt32(CODEC_OPUS);
            vector<uint32_t> decoders = config.enableVideoReceive ? video::VideoRenderer::GetAvailableDecoders() : vector<uint32_t>();
            vector<uint32_t> encoders = config.enableVideoSend ? video::VideoSource::GetAvailableEncoders() : vector<uint32_t>();
            out.WriteByte((unsigned char)decoders.size());
            for (uint32_t id : decoders)
            {
                out.WriteInt32(id);
            }
            if (connectionMaxLayer >= 92)
                out.WriteByte((unsigned char)video::VideoRenderer::GetMaximumResolution());
            else
                out.WriteByte(0);
        }
        SendOrEnqueuePacket(PendingOutgoingPacket{
            /*.seq=*/initSeq,
            /*.type=*/PKT_INIT,
            /*.len=*/out.GetLength(),
            /*.data=*/Buffer(move(out)),
            /*.endpoint=*/e.id});
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


void VoIPController::TrySendQueuedPackets()
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
            SendOrEnqueuePacket(move(*opkt), false);
            opkt = sendQueue.erase(opkt);
        }
        else
        {
            ++opkt;
        }
    }
}

bool VoIPController::WasOutgoingPacketAcknowledged(uint32_t seq)
{
    RecentOutgoingPacket *pkt = GetRecentOutgoingPacket(seq);
    if (!pkt)
        return false;
    return pkt->ackTime != 0.0;
}

VoIPController::RecentOutgoingPacket *VoIPController::GetRecentOutgoingPacket(uint32_t seq)
{
    for (RecentOutgoingPacket &opkt : recentOutgoingPackets)
    {
        if (opkt.seq == seq)
        {
            return &opkt;
        }
    }
    return NULL;
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