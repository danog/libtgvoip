#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

#pragma mark - Networking & crypto

void VoIPController::WritePacketHeader(uint32_t pseq, BufferOutputStream *s, unsigned char type, uint32_t length, PacketSender *source)
{
    uint32_t acks = 0;
    uint32_t distance;
    for (const uint32_t &seq : recentIncomingSeqs)
    {
        distance = lastRemoteSeq - seq;
        if (distance > 0 && distance <= 32)
        {
            acks |= (1 << (32 - distance));
        }
    }

    if (peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92))
    {
        s->WriteByte(type);
        s->WriteInt32(lastRemoteSeq);
        s->WriteInt32(pseq);
        s->WriteInt32(acks);
        unsigned char flags;
        if (currentExtras.empty())
        {
            flags = 0;
        }
        else
        {
            flags = XPFLAG_HAS_EXTRA;
        }

        shared_ptr<Stream> videoStream = GetStreamByType(STREAM_TYPE_VIDEO, false);
        if (peerVersion >= 9 && videoStream && videoStream->enabled)
            flags |= XPFLAG_HAS_RECV_TS;

        s->WriteByte(flags);

        if (!currentExtras.empty())
        {
            s->WriteByte(static_cast<unsigned char>(currentExtras.size()));
            for (vector<UnacknowledgedExtraData>::iterator x = currentExtras.begin(); x != currentExtras.end(); ++x)
            {
                LOGV("Writing extra into header: type %u, length %d", x->type, int(x->data.Length()));
                assert(x->data.Length() <= 254);
                s->WriteByte(static_cast<unsigned char>(x->data.Length() + 1));
                s->WriteByte(x->type);
                s->WriteBytes(*x->data, x->data.Length());
                if (x->firstContainingSeq == 0)
                    x->firstContainingSeq = pseq;
            }
        }
        if (peerVersion >= 9 && videoStream && videoStream->enabled)
        {
            s->WriteInt32(static_cast<uint32_t>((lastRecvPacketTime - connectionInitTime) * 1000.0));
        }
    }
    else
    {
        if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
        {
            s->WriteInt32(TLID_DECRYPTED_AUDIO_BLOCK);
            int64_t randomID;
            crypto.rand_bytes((uint8_t *)&randomID, 8);
            s->WriteInt64(randomID);
            unsigned char randBytes[7];
            crypto.rand_bytes(randBytes, 7);
            s->WriteByte(7);
            s->WriteBytes(randBytes, 7);
            uint32_t pflags = PFLAG_HAS_RECENT_RECV | PFLAG_HAS_SEQ;
            if (length > 0)
                pflags |= PFLAG_HAS_DATA;
            if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
            {
                pflags |= PFLAG_HAS_CALL_ID | PFLAG_HAS_PROTO;
            }
            pflags |= ((uint32_t)type) << 24;
            s->WriteInt32(pflags);

            if (pflags & PFLAG_HAS_CALL_ID)
            {
                s->WriteBytes(callID, 16);
            }
            s->WriteInt32(lastRemoteSeq);
            s->WriteInt32(pseq);
            s->WriteInt32(acks);
            if (pflags & PFLAG_HAS_PROTO)
            {
                s->WriteInt32(PROTOCOL_NAME);
            }
            if (length > 0)
            {
                if (length <= 253)
                {
                    s->WriteByte((unsigned char)length);
                }
                else
                {
                    s->WriteByte(254);
                    s->WriteByte((unsigned char)(length & 0xFF));
                    s->WriteByte((unsigned char)((length >> 8) & 0xFF));
                    s->WriteByte((unsigned char)((length >> 16) & 0xFF));
                }
            }
        }
        else
        {
            s->WriteInt32(TLID_SIMPLE_AUDIO_BLOCK);
            int64_t randomID;
            crypto.rand_bytes((uint8_t *)&randomID, 8);
            s->WriteInt64(randomID);
            unsigned char randBytes[7];
            crypto.rand_bytes(randBytes, 7);
            s->WriteByte(7);
            s->WriteBytes(randBytes, 7);
            uint32_t lenWithHeader = length + 13;
            if (lenWithHeader > 0)
            {
                if (lenWithHeader <= 253)
                {
                    s->WriteByte((unsigned char)lenWithHeader);
                }
                else
                {
                    s->WriteByte(254);
                    s->WriteByte((unsigned char)(lenWithHeader & 0xFF));
                    s->WriteByte((unsigned char)((lenWithHeader >> 8) & 0xFF));
                    s->WriteByte((unsigned char)((lenWithHeader >> 16) & 0xFF));
                }
            }
            s->WriteByte(type);
            s->WriteInt32(lastRemoteSeq);
            s->WriteInt32(pseq);
            s->WriteInt32(acks);
            if (peerVersion >= 6)
            {
                if (currentExtras.empty())
                {
                    s->WriteByte(0);
                }
                else
                {
                    s->WriteByte(XPFLAG_HAS_EXTRA);
                    s->WriteByte(static_cast<unsigned char>(currentExtras.size()));
                    for (vector<UnacknowledgedExtraData>::iterator x = currentExtras.begin(); x != currentExtras.end(); ++x)
                    {
                        LOGV("Writing extra into header: type %u, length %d", x->type, int(x->data.Length()));
                        assert(x->data.Length() <= 254);
                        s->WriteByte(static_cast<unsigned char>(x->data.Length() + 1));
                        s->WriteByte(x->type);
                        s->WriteBytes(*x->data, x->data.Length());
                        if (x->firstContainingSeq == 0)
                            x->firstContainingSeq = pseq;
                    }
                }
            }
        }
    }

    unacknowledgedIncomingPacketCount = 0;
    recentOutgoingPackets.push_back(RecentOutgoingPacket{
        pseq,
        0,
        GetCurrentTime(),
        0,
        type,
        length,
        source,
        false});
    while (recentOutgoingPackets.size() > MAX_RECENT_PACKETS)
    {
        recentOutgoingPackets.erase(recentOutgoingPackets.begin());
    }
    lastSentSeq = pseq;
    //LOGI("packet header size %d", s->GetLength());
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

void VoIPController::RunRecvThread()
{
    LOGI("Receive thread starting");
    if (proxyProtocol == PROXY_SOCKS5)
    {
        resolvedProxyAddress = NetworkSocket::ResolveDomainName(proxyAddress);
        if (resolvedProxyAddress.IsEmpty())
        {
            LOGW("Error resolving proxy address %s", proxyAddress.c_str());
            SetState(STATE_FAILED);
            return;
        }
    }
    else
    {
        udpConnectivityState = UDP_PING_PENDING;
        udpPingTimeoutID = messageThread.Post(std::bind(&VoIPController::SendUdpPings, this), 0.0, 0.5);
    }
    while (runReceiver)
    {
        if (proxyProtocol == PROXY_SOCKS5 && needReInitUdpProxy)
        {
            InitUDPProxy();
            needReInitUdpProxy = false;
        }

        vector<std::shared_ptr<NetworkSocket>> readSockets;
        vector<std::shared_ptr<NetworkSocket>> errorSockets;
        vector<std::shared_ptr<NetworkSocket>> writeSockets;
        readSockets.push_back(udpSocket);
        errorSockets.push_back(realUdpSocket);
        if (!realUdpSocket->IsReadyToSend())
            writeSockets.push_back(realUdpSocket);

        {
            MutexGuard m(endpointsMutex);
            for (pair<const int64_t, Endpoint> &_e : endpoints)
            {
                const Endpoint &e = _e.second;
                if (e.type == Endpoint::Type::TCP_RELAY)
                {
                    if (e.socket)
                    {
                        readSockets.push_back(e.socket);
                        errorSockets.push_back(e.socket);
                        if (!e.socket->IsReadyToSend())
                        {
                            NetworkSocketSOCKS5Proxy *proxy = dynamic_cast<NetworkSocketSOCKS5Proxy *>(&*e.socket);
                            if (!proxy || proxy->NeedSelectForSending())
                                writeSockets.push_back(e.socket);
                        }
                    }
                }
            }
        }

        {
            bool selRes = NetworkSocket::Select(readSockets, writeSockets, errorSockets, selectCanceller);
            if (!selRes)
            {
                LOGV("Select canceled");
                continue;
            }
        }
        if (!runReceiver)
            return;

        if (!errorSockets.empty())
        {
            if (find(errorSockets.begin(), errorSockets.end(), realUdpSocket) != errorSockets.end())
            {
                LOGW("UDP socket failed");
                SetState(STATE_FAILED);
                return;
            }
            MutexGuard m(endpointsMutex);
            for (std::shared_ptr<NetworkSocket> &socket : errorSockets)
            {
                for (pair<const int64_t, Endpoint> &_e : endpoints)
                {
                    Endpoint &e = _e.second;
                    if (e.socket == socket)
                    {
                        e.socket->Close();
                        e.socket.reset();
                        LOGI("Closing failed TCP socket for %s:%u", e.GetAddress().ToString().c_str(), e.port);
                    }
                }
            }
            continue;
        }

        for (std::shared_ptr<NetworkSocket> &socket : readSockets)
        {
            //while(packet.length){
            NetworkPacket packet = socket->Receive();
            if (packet.address.IsEmpty())
            {
                LOGE("Packet has null address. This shouldn't happen.");
                continue;
            }
            if (packet.data.IsEmpty())
            {
                LOGE("Packet has zero length.");
                continue;
            }
            //LOGV("Received %d bytes from %s:%d at %.5lf", len, packet.address->ToString().c_str(), packet.port, GetCurrentTime());
            messageThread.Post(bind(&VoIPController::NetworkPacketReceived, this, make_shared<NetworkPacket>(move(packet))));
        }

        if (!writeSockets.empty())
        {
            messageThread.Post(bind(&VoIPController::TrySendQueuedPackets, this));
        }
    }
    LOGI("=== recv thread exiting ===");
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

void VoIPController::NetworkPacketReceived(shared_ptr<NetworkPacket> _packet)
{
    ENFORCE_MSG_THREAD;

    NetworkPacket &packet = *_packet;

    int64_t srcEndpointID = 0;

    if (!packet.address.isIPv6)
    {
        for (pair<const int64_t, Endpoint> &_e : endpoints)
        {
            const Endpoint &e = _e.second;
            if (e.address == packet.address && e.port == packet.port)
            {
                if ((e.type != Endpoint::Type::TCP_RELAY && packet.protocol == NetworkProtocol::UDP) || (e.type == Endpoint::Type::TCP_RELAY && packet.protocol == NetworkProtocol::TCP))
                {
                    srcEndpointID = e.id;
                    break;
                }
            }
        }
        if (!srcEndpointID && packet.protocol == NetworkProtocol::UDP)
        {
            try
            {
                Endpoint &p2p = GetEndpointByType(Endpoint::Type::UDP_P2P_INET);
                if (p2p.rtts[0] == 0.0 && p2p.address.PrefixMatches(24, packet.address))
                {
                    LOGD("Packet source matches p2p endpoint partially: %s:%u", packet.address.ToString().c_str(), packet.port);
                    srcEndpointID = p2p.id;
                }
            }
            catch (out_of_range &ex)
            {
            }
        }
    }
    else
    {
        for (pair<const int64_t, Endpoint> &_e : endpoints)
        {
            const Endpoint &e = _e.second;
            if (e.v6address == packet.address && e.port == packet.port && e.IsIPv6Only())
            {
                if ((e.type != Endpoint::Type::TCP_RELAY && packet.protocol == NetworkProtocol::UDP) || (e.type == Endpoint::Type::TCP_RELAY && packet.protocol == NetworkProtocol::TCP))
                {
                    srcEndpointID = e.id;
                    break;
                }
            }
        }
    }

    if (!srcEndpointID)
    {
        LOGW("Received a packet from unknown source %s:%u", packet.address.ToString().c_str(), packet.port);
        return;
    }
    /*if(len<=0){
        //LOGW("error receiving: %d / %s", errno, strerror(errno));
        continue;
    }*/
    if (IS_MOBILE_NETWORK(networkType))
        stats.bytesRecvdMobile += (uint64_t)packet.data.Length();
    else
        stats.bytesRecvdWifi += (uint64_t)packet.data.Length();
    try
    {
        ProcessIncomingPacket(packet, endpoints.at(srcEndpointID));
    }
    catch (out_of_range &x)
    {
        LOGW("Error parsing packet: %s", x.what());
    }
}

void VoIPController::ProcessIncomingPacket(NetworkPacket &packet, Endpoint &srcEndpoint)
{
    ENFORCE_MSG_THREAD;

    // Initial packet decryption and recognition

    unsigned char *buffer = *packet.data;
    size_t len = packet.data.Length();
    BufferInputStream in(packet.data);
    bool hasPeerTag = false;
    if (peerVersion < 9 || srcEndpoint.IsReflector())
    {
        if (memcmp(buffer, srcEndpoint.IsReflector() ? (void *)srcEndpoint.peerTag : (void *)callID, 16) != 0)
        {
            LOGW("Received packet has wrong peerTag");
            return;
        }
        in.Seek(16);
        hasPeerTag = true;
    }
    if (in.Remaining() >= 16 && srcEndpoint.IsReflector() && *reinterpret_cast<uint64_t *>(buffer + 16) == 0xFFFFFFFFFFFFFFFFLL && *reinterpret_cast<uint32_t *>(buffer + 24) == 0xFFFFFFFF)
    {
        // relay special request response
        in.Seek(16 + 12);
        uint32_t tlid = in.ReadUInt32();

        if (tlid == TLID_UDP_REFLECTOR_SELF_INFO)
        {
            if (srcEndpoint.type == Endpoint::Type::UDP_RELAY /*&& udpConnectivityState==UDP_PING_SENT*/ && in.Remaining() >= 32)
            {
                int32_t date = in.ReadInt32();
                int64_t queryID = in.ReadInt64();
                unsigned char myIP[16];
                in.ReadBytes(myIP, 16);
                int32_t myPort = in.ReadInt32();
                //udpConnectivityState=UDP_AVAILABLE;
                double selfRTT = 0.0;
                srcEndpoint.udpPongCount++;
                srcEndpoint.totalUdpPingReplies++;
                if (srcEndpoint.udpPingTimes.find(queryID) != srcEndpoint.udpPingTimes.end())
                {
                    double sendTime = srcEndpoint.udpPingTimes[queryID];
                    srcEndpoint.udpPingTimes.erase(queryID);
                    srcEndpoint.selfRtts.Add(selfRTT = GetCurrentTime() - sendTime);
                }
                LOGV("Received UDP ping reply from %s:%d: date=%d, queryID=%ld, my IP=%s, my port=%d, selfRTT=%f", srcEndpoint.address.ToString().c_str(), srcEndpoint.port, date, (long int)queryID, NetworkAddress::IPv4(*reinterpret_cast<uint32_t *>(myIP + 12)).ToString().c_str(), myPort, selfRTT);
                if (srcEndpoint.IsIPv6Only() && !didSendIPv6Endpoint)
                {
                    NetworkAddress realAddr = NetworkAddress::IPv6(myIP);
                    if (realAddr == myIPv6)
                    {
                        LOGI("Public IPv6 matches local address");
                        useIPv6 = true;
                        if (allowP2p)
                        {
                            didSendIPv6Endpoint = true;
                            BufferOutputStream o(18);
                            o.WriteBytes(myIP, 16);
                            o.WriteInt16(udpSocket->GetLocalPort());
                            Buffer b(move(o));
                            SendExtra(b, EXTRA_TYPE_IPV6_ENDPOINT);
                        }
                    }
                }
            }
        }
        else if (tlid == TLID_UDP_REFLECTOR_PEER_INFO)
        {
            if (in.Remaining() >= 16)
            {
                uint32_t myAddr = in.ReadUInt32();
                uint32_t myPort = in.ReadUInt32();
                uint32_t peerAddr = in.ReadUInt32();
                uint32_t peerPort = in.ReadUInt32();

                constexpr int64_t p2pID = static_cast<int64_t>(FOURCC('P', '2', 'P', '4')) << 32;
                constexpr int64_t lanID = static_cast<int64_t>(FOURCC('L', 'A', 'N', '4')) << 32;

                if (currentEndpoint == p2pID || currentEndpoint == lanID)
                    currentEndpoint = preferredRelay;

                if (endpoints.find(lanID) != endpoints.end())
                {
                    MutexGuard m(endpointsMutex);
                    endpoints.erase(lanID);
                }

                unsigned char peerTag[16];
                LOGW("Received reflector peer info, my=%s:%u, peer=%s:%u", NetworkAddress::IPv4(myAddr).ToString().c_str(), myPort, NetworkAddress::IPv4(peerAddr).ToString().c_str(), peerPort);
                if (waitingForRelayPeerInfo)
                {
                    Endpoint p2p(p2pID, (uint16_t)peerPort, NetworkAddress::IPv4(peerAddr), NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_INET, peerTag);
                    {
                        MutexGuard m(endpointsMutex);
                        endpoints[p2pID] = p2p;
                    }
                    if (myAddr == peerAddr)
                    {
                        LOGW("Detected LAN");
                        NetworkAddress lanAddr = NetworkAddress::IPv4(0);
                        udpSocket->GetLocalInterfaceInfo(&lanAddr, NULL);

                        BufferOutputStream pkt(8);
                        pkt.WriteInt32(lanAddr.addr.ipv4);
                        pkt.WriteInt32(udpSocket->GetLocalPort());
                        if (peerVersion < 6)
                        {
                            SendPacketReliably(PKT_LAN_ENDPOINT, pkt.GetBuffer(), pkt.GetLength(), 0.5, 10);
                        }
                        else
                        {
                            Buffer buf(move(pkt));
                            SendExtra(buf, EXTRA_TYPE_LAN_ENDPOINT);
                        }
                    }
                    waitingForRelayPeerInfo = false;
                }
            }
        }
        else
        {
            LOGV("Received relay response with unknown tl id: 0x%08X", tlid);
        }
        return;
    }
    if (in.Remaining() < 40)
    {
        LOGV("Received packet is too small");
        return;
    }

    bool retryWith2 = false;
    size_t innerLen = 0;
    bool shortFormat = peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92);

    if (!useMTProto2)
    {
        unsigned char fingerprint[8], msgHash[16];
        in.ReadBytes(fingerprint, 8);
        in.ReadBytes(msgHash, 16);
        unsigned char key[32], iv[32];
        KDF(msgHash, isOutgoing ? 8 : 0, key, iv);
        unsigned char aesOut[MSC_STACK_FALLBACK(in.Remaining(), 1500)];
        if (in.Remaining() > sizeof(aesOut))
            return;
        crypto.aes_ige_decrypt((unsigned char *)buffer + in.GetOffset(), aesOut, in.Remaining(), key, iv);
        BufferInputStream _in(aesOut, in.Remaining());
        unsigned char sha[SHA1_LENGTH];
        uint32_t _len = _in.ReadUInt32();
        if (_len > _in.Remaining())
            _len = (uint32_t)_in.Remaining();
        crypto.sha1((uint8_t *)(aesOut), (size_t)(_len + 4), sha);
        if (memcmp(msgHash, sha + (SHA1_LENGTH - 16), 16) != 0)
        {
            LOGW("Received packet has wrong hash after decryption");
            if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
                retryWith2 = true;
            else
                return;
        }
        else
        {
            memcpy(buffer + in.GetOffset(), aesOut, in.Remaining());
            in.ReadInt32();
        }
    }

    if (useMTProto2 || retryWith2)
    {
        if (hasPeerTag)
            in.Seek(16); // peer tag

        unsigned char fingerprint[8], msgKey[16];
        if (!shortFormat)
        {
            in.ReadBytes(fingerprint, 8);
            if (memcmp(fingerprint, keyFingerprint, 8) != 0)
            {
                LOGW("Received packet has wrong key fingerprint");
                return;
            }
        }
        in.ReadBytes(msgKey, 16);

        unsigned char decrypted[1500];
        unsigned char aesKey[32], aesIv[32];
        KDF2(msgKey, isOutgoing ? 8 : 0, aesKey, aesIv);
        size_t decryptedLen = in.Remaining();
        if (decryptedLen > sizeof(decrypted))
            return;
        if (decryptedLen % 16 != 0)
        {
            LOGW("wrong decrypted length");
            return;
        }

        crypto.aes_ige_decrypt(*packet.data + in.GetOffset(), decrypted, decryptedLen, aesKey, aesIv);

        in = BufferInputStream(decrypted, decryptedLen);
        //LOGD("received packet length: %d", in.ReadInt32());
        size_t sizeSize = shortFormat ? 0 : 4;

        BufferOutputStream buf(decryptedLen + 32);
        size_t x = isOutgoing ? 8 : 0;
        buf.WriteBytes(encryptionKey + 88 + x, 32);
        buf.WriteBytes(decrypted + sizeSize, decryptedLen - sizeSize);
        unsigned char msgKeyLarge[32];
        crypto.sha256(buf.GetBuffer(), buf.GetLength(), msgKeyLarge);

        if (memcmp(msgKey, msgKeyLarge + 8, 16) != 0)
        {
            LOGW("Received packet has wrong hash");
            return;
        }

        innerLen = static_cast<uint32_t>(shortFormat ? in.ReadInt16() : in.ReadInt32());
        if (innerLen > decryptedLen - sizeSize)
        {
            LOGW("Received packet has wrong inner length (%d with total of %u)", (int)innerLen, (unsigned int)decryptedLen);
            return;
        }
        if (decryptedLen - innerLen < (shortFormat ? 16 : 12))
        {
            LOGW("Received packet has too little padding (%u)", (unsigned int)(decryptedLen - innerLen));
            return;
        }
        memcpy(buffer, decrypted + (shortFormat ? 2 : 4), innerLen);
        in = BufferInputStream(buffer, (size_t)innerLen);
        if (retryWith2)
        {
            LOGD("Successfully decrypted packet in MTProto2.0 fallback, upgrading");
            useMTProto2 = true;
        }
    }

    lastRecvPacketTime = GetCurrentTime();

    if (state == STATE_RECONNECTING)
    {
        LOGI("Received a valid packet while reconnecting - setting state to established");
        SetState(STATE_ESTABLISHED);
    }

    if (srcEndpoint.type == Endpoint::Type::UDP_P2P_INET && !srcEndpoint.IsIPv6Only())
    {
        if (srcEndpoint.port != packet.port || srcEndpoint.address != packet.address)
        {
            if (!packet.address.isIPv6)
            {
                LOGI("Incoming packet was decrypted successfully, changing P2P endpoint to %s:%u", packet.address.ToString().c_str(), packet.port);
                srcEndpoint.address = packet.address;
                srcEndpoint.port = packet.port;
            }
        }
    }

    // decryptedAudioBlock random_id:long random_bytes:string flags:# voice_call_id:flags.2?int128 in_seq_no:flags.4?int out_seq_no:flags.4?int
    //   recent_received_mask:flags.5?int proto:flags.3?int extra:flags.1?string raw_data:flags.0?string = DecryptedAudioBlock
    //
    // simpleAudioBlock random_id:long random_bytes:string raw_data:string = DecryptedAudioBlock;

    // Version-specific extraction of packet fields ackId (last received packet seq on remote), (incoming packet seq) pseq, (ack mask) acks, (packet type) type, (flags) pflags, packet length
    uint32_t ackId;             // Last received packet seqno on remote
    uint32_t pseq;              // Incoming packet seqno
    uint32_t acks;              // Ack mask
    unsigned char type, pflags; // Packet type, flags
    size_t packetInnerLen = 0;
    if (shortFormat)
    {
        type = in.ReadByte();
        ackId = in.ReadUInt32();
        pseq = in.ReadUInt32();
        acks = in.ReadUInt32();
        pflags = in.ReadByte();
        packetInnerLen = innerLen - 14;
    }
    else
    {
        uint32_t tlid = in.ReadUInt32();
        if (tlid == TLID_DECRYPTED_AUDIO_BLOCK)
        {
            in.ReadInt64(); // random id
            uint32_t randLen = in.ReadTlLength();
            in.Seek(in.GetOffset() + randLen + pad4(randLen));
            uint32_t flags = in.ReadUInt32();
            type = (unsigned char)((flags >> 24) & 0xFF);
            if (!(flags & PFLAG_HAS_SEQ && flags & PFLAG_HAS_RECENT_RECV))
            {
                LOGW("Received packet doesn't have PFLAG_HAS_SEQ, PFLAG_HAS_RECENT_RECV, or both");

                return;
            }
            if (flags & PFLAG_HAS_CALL_ID)
            {
                unsigned char pktCallID[16];
                in.ReadBytes(pktCallID, 16);
                if (memcmp(pktCallID, callID, 16) != 0)
                {
                    LOGW("Received packet has wrong call id");

                    lastError = ERROR_UNKNOWN;
                    SetState(STATE_FAILED);
                    return;
                }
            }
            ackId = in.ReadUInt32();
            pseq = in.ReadUInt32();
            acks = in.ReadUInt32();
            if (flags & PFLAG_HAS_PROTO)
            {
                uint32_t proto = in.ReadUInt32();
                if (proto != PROTOCOL_NAME)
                {
                    LOGW("Received packet uses wrong protocol");

                    lastError = ERROR_INCOMPATIBLE;
                    SetState(STATE_FAILED);
                    return;
                }
            }
            if (flags & PFLAG_HAS_EXTRA)
            {
                uint32_t extraLen = in.ReadTlLength();
                in.Seek(in.GetOffset() + extraLen + pad4(extraLen));
            }
            if (flags & PFLAG_HAS_DATA)
            {
                packetInnerLen = in.ReadTlLength();
            }
            pflags = 0;
        }
        else if (tlid == TLID_SIMPLE_AUDIO_BLOCK)
        {
            in.ReadInt64(); // random id
            uint32_t randLen = in.ReadTlLength();
            in.Seek(in.GetOffset() + randLen + pad4(randLen));
            packetInnerLen = in.ReadTlLength();
            type = in.ReadByte();
            ackId = in.ReadUInt32();
            pseq = in.ReadUInt32();
            acks = in.ReadUInt32();
            if (peerVersion >= 6)
                pflags = in.ReadByte();
            else
                pflags = 0;
        }
        else
        {
            LOGW("Received a packet of unknown type %08X", tlid);

            return;
        }
    }
    packetsReceived++;

    // Duplicate and moving window check
    if (seqgt(pseq, lastRemoteSeq - MAX_RECENT_PACKETS))
    {
        if (find(recentIncomingSeqs.begin(), recentIncomingSeqs.end(), pseq) != recentIncomingSeqs.end())
        {
            LOGW("Received duplicated packet for seq %u", pseq);
            return;
        }
        recentIncomingSeqs[recentIncomingSeqIdx++] = pseq;
        recentIncomingSeqIdx %= recentIncomingSeqs.size();

        if (seqgt(pseq, lastRemoteSeq))
            lastRemoteSeq = pseq;
    }
    else
    {
        LOGW("Packet %u is out of order and too late", pseq);
        return;
    }

    // Extra data
    if (pflags & XPFLAG_HAS_EXTRA)
    {
        unsigned char extraCount = in.ReadByte();
        for (int i = 0; i < extraCount; i++)
        {
            size_t extraLen = in.ReadByte();
            Buffer xbuffer(extraLen);
            in.ReadBytes(*xbuffer, extraLen);
            ProcessExtraData(xbuffer);
        }
    }

    uint32_t recvTS = 0;
    if (pflags & XPFLAG_HAS_RECV_TS)
    {
        recvTS = static_cast<uint32_t>(in.ReadInt32());
    }
    if (seqgt(ackId, lastRemoteAckSeq))
    {

        if (waitingForAcks && lastRemoteAckSeq >= firstSentPing)
        {
            rttHistory.Reset();
            waitingForAcks = false;
            dontSendPackets = 10;
            messageThread.Post(
                [this] {
                    dontSendPackets = 0;
                },
                1.0);
            LOGI("resuming sending");
        }
        lastRemoteAckSeq = ackId;
        conctl.PacketAcknowledged(ackId);

        // Status list of acked seqnos, starting from the seq explicitly present in the packet + up to 32 seqs ago
        std::array<uint32_t, 33> peerAcks{0};
        peerAcks[0] = ackId;
        for (unsigned int i = 1; i <= 32; i++)
        {
            if ((acks >> (32 - i)) & 1)
            {
                peerAcks[i] = ackId - i;
            }
        }

        for (RecentOutgoingPacket &opkt : recentOutgoingPackets)
        {
            if (opkt.ackTime != 0.0)
                continue;
            if (find(peerAcks.begin(), peerAcks.end(), opkt.seq) != peerAcks.end())
            {
                opkt.ackTime = GetCurrentTime();
                if (opkt.lost)
                {
                    LOGW("acknowledged lost packet %u", opkt.seq);
                    sendLosses--;
                }
                if (opkt.sender && !opkt.lost)
                { // don't report lost packets as acknowledged to PacketSenders
                    opkt.sender->PacketAcknowledged(opkt.seq, opkt.sendTime, recvTS / 1000.0f, opkt.type, opkt.size);
                }

                // TODO move this to a PacketSender
                conctl.PacketAcknowledged(opkt.seq);
            }
        }

        if (peerVersion < 6)
        {
            for (unsigned int i = 0; i < queuedPackets.size(); i++)
            {
                QueuedPacket &qp = queuedPackets[i];
                int j;
                bool didAck = false;
                for (j = 0; j < 16; j++)
                {
                    LOGD("queued packet %u, seq %u=%u", i, j, qp.seqs[j]);
                    if (qp.seqs[j] == 0)
                        break;
                    int remoteAcksIndex = lastRemoteAckSeq - qp.seqs[j];
                    //LOGV("remote acks index %u, value %f", remoteAcksIndex, remoteAcksIndex>=0 && remoteAcksIndex<32 ? remoteAcks[remoteAcksIndex] : -1);
                    if (seqgt(lastRemoteAckSeq, qp.seqs[j]) && remoteAcksIndex >= 0 && remoteAcksIndex < 32)
                    {
                        for (RecentOutgoingPacket &opkt : recentOutgoingPackets)
                        {
                            if (opkt.seq == qp.seqs[j] && opkt.ackTime > 0)
                            {
                                LOGD("did ack seq %u, removing", qp.seqs[j]);
                                didAck = true;
                                break;
                            }
                        }
                        if (didAck)
                            break;
                    }
                }
                if (didAck)
                {
                    queuedPackets.erase(queuedPackets.begin() + i);
                    i--;
                    continue;
                }
            }
        }
        else
        {
            for (auto x = currentExtras.begin(); x != currentExtras.end();)
            {
                if (x->firstContainingSeq != 0 && seqgte(lastRemoteAckSeq, x->firstContainingSeq))
                {
                    LOGV("Peer acknowledged extra type %u length %u", x->type, (unsigned int)x->data.Length());
                    ProcessAcknowledgedOutgoingExtra(*x);
                    x = currentExtras.erase(x);
                    continue;
                }
                ++x;
            }
        }
    }

    Endpoint *_currentEndpoint = &endpoints.at(currentEndpoint);
    if (srcEndpoint.id != currentEndpoint && srcEndpoint.IsReflector() && (_currentEndpoint->IsP2P() || _currentEndpoint->averageRTT == 0))
    {
        if (seqgt(lastSentSeq - 32, lastRemoteAckSeq))
        {
            currentEndpoint = srcEndpoint.id;
            _currentEndpoint = &srcEndpoint;
            LOGI("Peer network address probably changed, switching to relay");
            if (allowP2p)
                SendPublicEndpointsRequest();
        }
    }

    if (config.logPacketStats)
    {
        DebugLoggedPacket dpkt = {
            static_cast<int32_t>(pseq),
            GetCurrentTime() - connectionInitTime,
            static_cast<int32_t>(packet.data.Length())};
        debugLoggedPackets.push_back(dpkt);
        if (debugLoggedPackets.size() >= 2500)
        {
            debugLoggedPackets.erase(debugLoggedPackets.begin(), debugLoggedPackets.begin() + 500);
        }
    }

    unacknowledgedIncomingPacketCount++;
    if (unacknowledgedIncomingPacketCount > unackNopThreshold)
    {
        //LOGV("Sending nop packet as ack");
        SendNopPacket();
    }

#ifdef LOG_PACKETS
    LOGV("Received: from=%s:%u, seq=%u, length=%u, type=%s", srcEndpoint.GetAddress().ToString().c_str(), srcEndpoint.port, pseq, (unsigned int)packet.data.Length(), GetPacketTypeString(type).c_str());
#endif

    //LOGV("acks: %u -> %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf", lastRemoteAckSeq, remoteAcks[0], remoteAcks[1], remoteAcks[2], remoteAcks[3], remoteAcks[4], remoteAcks[5], remoteAcks[6], remoteAcks[7]);
    //LOGD("recv: %u -> %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf", lastRemoteSeq, recvPacketTimes[0], recvPacketTimes[1], recvPacketTimes[2], recvPacketTimes[3], recvPacketTimes[4], recvPacketTimes[5], recvPacketTimes[6], recvPacketTimes[7]);
    //LOGI("RTT = %.3lf", GetAverageRTT());
    //LOGV("Packet %u type is %d", pseq, type);
    if (type == PKT_INIT)
    {
        LOGD("Received init");
        uint32_t ver = in.ReadUInt32();
        if (!receivedInit)
            peerVersion = ver;
        LOGI("Peer version is %d", peerVersion);
        uint32_t minVer = in.ReadUInt32();
        if (minVer > PROTOCOL_VERSION || peerVersion < MIN_PROTOCOL_VERSION)
        {
            lastError = ERROR_INCOMPATIBLE;

            SetState(STATE_FAILED);
            return;
        }
        uint32_t flags = in.ReadUInt32();
        if (!receivedInit)
        {
            if (flags & INIT_FLAG_DATA_SAVING_ENABLED)
            {
                dataSavingRequestedByPeer = true;
                UpdateDataSavingState();
                UpdateAudioBitrateLimit();
            }
            if (flags & INIT_FLAG_GROUP_CALLS_SUPPORTED)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_GROUP_CALLS;
            }
            if (flags & INIT_FLAG_VIDEO_RECV_SUPPORTED)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_VIDEO_DISPLAY;
            }
            if (flags & INIT_FLAG_VIDEO_SEND_SUPPORTED)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_VIDEO_CAPTURE;
            }
        }

        unsigned int i;
        unsigned int numSupportedAudioCodecs = in.ReadByte();
        for (i = 0; i < numSupportedAudioCodecs; i++)
        {
            if (peerVersion < 5)
                in.ReadByte(); // ignore for now
            else
                in.ReadInt32();
        }
        if (!receivedInit && ((flags & INIT_FLAG_VIDEO_SEND_SUPPORTED && config.enableVideoReceive) || (flags & INIT_FLAG_VIDEO_RECV_SUPPORTED && config.enableVideoSend)))
        {
            LOGD("Peer video decoders:");
            unsigned int numSupportedVideoDecoders = in.ReadByte();
            for (i = 0; i < numSupportedVideoDecoders; i++)
            {
                uint32_t id = static_cast<uint32_t>(in.ReadInt32());
                peerVideoDecoders.push_back(id);
                char *_id = reinterpret_cast<char *>(&id);
                LOGD("%c%c%c%c", _id[3], _id[2], _id[1], _id[0]);
            }
            protocolInfo.maxVideoResolution = in.ReadByte();

            SetupOutgoingVideoStream();
        }

        BufferOutputStream out(1024);

        out.WriteInt32(PROTOCOL_VERSION);
        out.WriteInt32(MIN_PROTOCOL_VERSION);

        out.WriteByte((unsigned char)outgoingStreams.size());
        for (vector<shared_ptr<Stream>>::iterator s = outgoingStreams.begin(); s != outgoingStreams.end(); ++s)
        {
            out.WriteByte((*s)->id);
            out.WriteByte((*s)->type);
            if (peerVersion < 5)
                out.WriteByte((unsigned char)((*s)->codec == CODEC_OPUS ? CODEC_OPUS_OLD : 0));
            else
                out.WriteInt32((*s)->codec);
            out.WriteInt16((*s)->frameDuration);
            out.WriteByte((unsigned char)((*s)->enabled ? 1 : 0));
        }
        LOGI("Sending init ack");
        size_t outLength = out.GetLength();
        SendOrEnqueuePacket(PendingOutgoingPacket{
            /*.seq=*/GenerateOutSeq(),
            /*.type=*/PKT_INIT_ACK,
            /*.len=*/outLength,
            /*.data=*/Buffer(move(out)),
            /*.endpoint=*/0});
        if (!receivedInit)
        {
            receivedInit = true;
            if ((srcEndpoint.type == Endpoint::Type::UDP_RELAY && udpConnectivityState != UDP_BAD && udpConnectivityState != UDP_NOT_AVAILABLE) || srcEndpoint.type == Endpoint::Type::TCP_RELAY)
            {
                currentEndpoint = srcEndpoint.id;
                if (srcEndpoint.type == Endpoint::Type::UDP_RELAY || (useTCP && srcEndpoint.type == Endpoint::Type::TCP_RELAY))
                    preferredRelay = srcEndpoint.id;
            }
        }
        if (!audioStarted && receivedInitAck)
        {
            StartAudio();
            audioStarted = true;
        }
    }
    if (type == PKT_INIT_ACK)
    {
        LOGD("Received init ack");

        if (!receivedInitAck)
        {
            receivedInitAck = true;

            messageThread.Cancel(initTimeoutID);
            initTimeoutID = MessageThread::INVALID_ID;

            if (packetInnerLen > 10)
            {
                peerVersion = in.ReadInt32();
                uint32_t minVer = in.ReadUInt32();
                if (minVer > PROTOCOL_VERSION || peerVersion < MIN_PROTOCOL_VERSION)
                {
                    lastError = ERROR_INCOMPATIBLE;

                    SetState(STATE_FAILED);
                    return;
                }
            }
            else
            {
                peerVersion = 1;
            }

            LOGI("peer version from init ack %d", peerVersion);

            unsigned char streamCount = in.ReadByte();
            if (streamCount == 0)
                return;

            int i;
            shared_ptr<Stream> incomingAudioStream = nullptr;
            for (i = 0; i < streamCount; i++)
            {
                shared_ptr<Stream> stm = make_shared<Stream>();
                stm->id = in.ReadByte();
                stm->type = in.ReadByte();
                if (peerVersion < 5)
                {
                    unsigned char codec = in.ReadByte();
                    if (codec == CODEC_OPUS_OLD)
                        stm->codec = CODEC_OPUS;
                }
                else
                {
                    stm->codec = in.ReadUInt32();
                }
                in.ReadInt16();
                stm->frameDuration = 60;
                stm->enabled = in.ReadByte() == 1;
                if (stm->type == STREAM_TYPE_VIDEO && peerVersion < 9)
                {
                    LOGV("Skipping video stream for old protocol version");
                    continue;
                }
                if (stm->type == STREAM_TYPE_AUDIO)
                {
                    stm->jitterBuffer = make_shared<JitterBuffer>(stm->frameDuration);
                    if (stm->frameDuration > 50)
                        stm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_60", 2));
                    else if (stm->frameDuration > 30)
                        stm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_40", 4));
                    else
                        stm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_20", 6));
                    stm->decoder = nullptr;
                }
                else if (stm->type == STREAM_TYPE_VIDEO)
                {
                    if (!stm->packetReassembler)
                    {
                        stm->packetReassembler = make_shared<PacketReassembler>();
                        stm->packetReassembler->SetCallback(bind(&VoIPController::ProcessIncomingVideoFrame, this, placeholders::_1, placeholders::_2, placeholders::_3, placeholders::_4));
                    }
                }
                else
                {
                    LOGW("Unknown incoming stream type: %d", stm->type);
                    continue;
                }
                incomingStreams.push_back(stm);
                if (stm->type == STREAM_TYPE_AUDIO && !incomingAudioStream)
                    incomingAudioStream = stm;
            }
            if (!incomingAudioStream)
                return;

            if (peerVersion >= 5 && !useMTProto2)
            {
                useMTProto2 = true;
                LOGD("MTProto2 wasn't initially enabled for whatever reason but peer supports it; upgrading");
            }

            if (!audioStarted && receivedInit)
            {
                StartAudio();
                audioStarted = true;
            }
            messageThread.Post(
                [this] {
                    if (state == STATE_WAIT_INIT_ACK)
                    {
                        SetState(STATE_ESTABLISHED);
                    }
                },
                ServerConfig::GetSharedInstance()->GetDouble("established_delay_if_no_stream_data", 1.5));
            if (allowP2p)
                SendPublicEndpointsRequest();
        }
    }
    if (type == PKT_STREAM_DATA || type == PKT_STREAM_DATA_X2 || type == PKT_STREAM_DATA_X3)
    {
        if (!receivedFirstStreamPacket)
        {
            receivedFirstStreamPacket = true;
            if (state != STATE_ESTABLISHED && receivedInitAck)
            {
                messageThread.Post(
                    [this]() {
                        SetState(STATE_ESTABLISHED);
                    },
                    .5);
                LOGW("First audio packet - setting state to ESTABLISHED");
            }
        }
        int count;
        switch (type)
        {
        case PKT_STREAM_DATA_X2:
            count = 2;
            break;
        case PKT_STREAM_DATA_X3:
            count = 3;
            break;
        case PKT_STREAM_DATA:
        default:
            count = 1;
            break;
        }
        int i;
        if (srcEndpoint.type == Endpoint::Type::UDP_RELAY && srcEndpoint.id != peerPreferredRelay)
        {
            peerPreferredRelay = srcEndpoint.id;
        }
        for (i = 0; i < count; i++)
        {
            unsigned char streamID = in.ReadByte();
            unsigned char flags = (unsigned char)(streamID & 0xC0);
            streamID &= 0x3F;
            uint16_t sdlen = (uint16_t)(flags & STREAM_DATA_FLAG_LEN16 ? in.ReadInt16() : in.ReadByte());
            uint32_t pts = in.ReadUInt32();
            unsigned char fragmentCount = 1;
            unsigned char fragmentIndex = 0;
            //LOGD("stream data, pts=%d, len=%d, rem=%d", pts, sdlen, in.Remaining());
            audioTimestampIn = pts;
            if (!audioOutStarted && audioOutput)
            {
                MutexGuard m(audioIOMutex);
                audioOutput->Start();
                audioOutStarted = true;
            }
            bool fragmented = static_cast<bool>(sdlen & STREAM_DATA_XFLAG_FRAGMENTED);
            bool extraFEC = static_cast<bool>(sdlen & STREAM_DATA_XFLAG_EXTRA_FEC);
            bool keyframe = static_cast<bool>(sdlen & STREAM_DATA_XFLAG_KEYFRAME);
            if (fragmented)
            {
                fragmentIndex = in.ReadByte();
                fragmentCount = in.ReadByte();
            }
            sdlen &= 0x7FF;
            if (in.GetOffset() + sdlen > len)
            {
                return;
            }
            shared_ptr<Stream> stm;
            for (shared_ptr<Stream> &ss : incomingStreams)
            {
                if (ss->id == streamID)
                {
                    stm = ss;
                    break;
                }
            }
            if (stm && stm->type == STREAM_TYPE_AUDIO)
            {
                if (stm->jitterBuffer)
                {
                    stm->jitterBuffer->HandleInput(static_cast<unsigned char *>(buffer + in.GetOffset()), sdlen, pts, false);
                    if (extraFEC)
                    {
                        in.Seek(in.GetOffset() + sdlen);
                        unsigned int fecCount = in.ReadByte();
                        for (unsigned int j = 0; j < fecCount; j++)
                        {
                            unsigned char dlen = in.ReadByte();
                            unsigned char data[256];
                            in.ReadBytes(data, dlen);
                            stm->jitterBuffer->HandleInput(data, dlen, pts - (fecCount - j - 1) * stm->frameDuration, true);
                        }
                    }
                }
            }
            else if (stm && stm->type == STREAM_TYPE_VIDEO)
            {
                if (stm->packetReassembler)
                {
                    uint8_t frameSeq = in.ReadByte();
                    Buffer pdata(sdlen);
                    uint16_t rotation = 0;
                    if (fragmentIndex == 0)
                    {
                        unsigned char _rotation = in.ReadByte() & (unsigned char)VIDEO_ROTATION_MASK;
                        switch (_rotation)
                        {
                        case VIDEO_ROTATION_0:
                            rotation = 0;
                            break;
                        case VIDEO_ROTATION_90:
                            rotation = 90;
                            break;
                        case VIDEO_ROTATION_180:
                            rotation = 180;
                            break;
                        case VIDEO_ROTATION_270:
                            rotation = 270;
                            break;
                        default: // unreachable on sane CPUs
                            abort();
                        }
                        //if(rotation!=stm->rotation){
                        //	stm->rotation=rotation;
                        //	LOGI("Video rotation: %u", rotation);
                        //}
                    }
                    pdata.CopyFrom(buffer + in.GetOffset(), 0, sdlen);
                    stm->packetReassembler->AddFragment(std::move(pdata), fragmentIndex, fragmentCount, pts, frameSeq, keyframe, rotation);
                }
                //LOGV("Received video fragment %u of %u", fragmentIndex, fragmentCount);
            }
            else
            {
                LOGW("received packet for unknown stream %u", (unsigned int)streamID);
            }
            if (i < count - 1)
                in.Seek(in.GetOffset() + sdlen);
        }
    }
    if (type == PKT_PING)
    {
        //LOGD("Received ping from %s:%d", srcEndpoint.address.ToString().c_str(), srcEndpoint.port);
        if (srcEndpoint.type != Endpoint::Type::UDP_RELAY && srcEndpoint.type != Endpoint::Type::TCP_RELAY && !allowP2p)
        {
            LOGW("Received p2p ping but p2p is disabled by manual override");
            return;
        }
        BufferOutputStream pkt(128);
        pkt.WriteInt32(pseq);
        size_t pktLength = pkt.GetLength();
        SendOrEnqueuePacket(PendingOutgoingPacket{
            /*.seq=*/GenerateOutSeq(),
            /*.type=*/PKT_PONG,
            /*.len=*/pktLength,
            /*.data=*/Buffer(move(pkt)),
            /*.endpoint=*/srcEndpoint.id,
        });
    }
    if (type == PKT_PONG)
    {
        if (packetInnerLen >= 4)
        {
            uint32_t pingSeq = in.ReadUInt32();
#ifdef LOG_PACKETS
            LOGD("Received pong for ping in seq %u", pingSeq);
#endif
            if (pingSeq == srcEndpoint.lastPingSeq)
            {
                srcEndpoint.rtts.Add(GetCurrentTime() - srcEndpoint.lastPingTime);
                srcEndpoint.averageRTT = srcEndpoint.rtts.NonZeroAverage();
                LOGD("Current RTT via %s: %.3f, average: %.3f", packet.address.ToString().c_str(), srcEndpoint.rtts[0], srcEndpoint.averageRTT);
                if (srcEndpoint.averageRTT > rateMaxAcceptableRTT)
                    needRate = true;
            }
        }
    }
    if (type == PKT_STREAM_STATE)
    {
        unsigned char id = in.ReadByte();
        unsigned char enabled = in.ReadByte();
        LOGV("Peer stream state: id %u flags %u", (int)id, (int)enabled);
        for (vector<shared_ptr<Stream>>::iterator s = incomingStreams.begin(); s != incomingStreams.end(); ++s)
        {
            if ((*s)->id == id)
            {
                (*s)->enabled = enabled == 1;
                UpdateAudioOutputState();
                break;
            }
        }
    }
    if (type == PKT_LAN_ENDPOINT)
    {
        LOGV("received lan endpoint");
        uint32_t peerAddr = in.ReadUInt32();
        uint16_t peerPort = (uint16_t)in.ReadInt32();
        constexpr int64_t lanID = static_cast<int64_t>(FOURCC('L', 'A', 'N', '4')) << 32;
        unsigned char peerTag[16];
        Endpoint lan(lanID, peerPort, NetworkAddress::IPv4(peerAddr), NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_LAN, peerTag);

        if (currentEndpoint == lanID)
            currentEndpoint = preferredRelay;

        MutexGuard m(endpointsMutex);
        endpoints[lanID] = lan;
    }
    if (type == PKT_NETWORK_CHANGED && _currentEndpoint->type != Endpoint::Type::UDP_RELAY && _currentEndpoint->type != Endpoint::Type::TCP_RELAY)
    {
        currentEndpoint = preferredRelay;
        if (allowP2p)
            SendPublicEndpointsRequest();
        if (peerVersion >= 2)
        {
            uint32_t flags = in.ReadUInt32();
            dataSavingRequestedByPeer = (flags & INIT_FLAG_DATA_SAVING_ENABLED) == INIT_FLAG_DATA_SAVING_ENABLED;
            UpdateDataSavingState();
            UpdateAudioBitrateLimit();
            ResetEndpointPingStats();
        }
    }
    if (type == PKT_STREAM_EC)
    {
        unsigned char streamID = in.ReadByte();
        if (peerVersion < 7)
        {
            uint32_t lastTimestamp = in.ReadUInt32();
            unsigned char count = in.ReadByte();
            for (shared_ptr<Stream> &stm : incomingStreams)
            {
                if (stm->id == streamID)
                {
                    for (unsigned int i = 0; i < count; i++)
                    {
                        unsigned char dlen = in.ReadByte();
                        unsigned char data[256];
                        in.ReadBytes(data, dlen);
                        if (stm->jitterBuffer)
                        {
                            stm->jitterBuffer->HandleInput(data, dlen, lastTimestamp - (count - i - 1) * stm->frameDuration, true);
                        }
                    }
                    break;
                }
            }
        }
        else
        {
            shared_ptr<Stream> stm = GetStreamByID(streamID, false);
            if (!stm)
            {
                LOGW("Received FEC packet for unknown stream %u", streamID);
                return;
            }
            if (stm->type != STREAM_TYPE_VIDEO)
            {
                LOGW("Received FEC packet for non-video stream %u", streamID);
                return;
            }
            if (!stm->packetReassembler)
                return;

            uint8_t fseq = in.ReadByte();
            unsigned char fecScheme = in.ReadByte();
            unsigned char prevFrameCount = in.ReadByte();
            uint16_t fecLen = in.ReadUInt16();
            if (fecLen > in.Remaining())
                return;

            Buffer fecData(fecLen);
            in.ReadBytes(fecData);

            stm->packetReassembler->AddFEC(std::move(fecData), fseq, prevFrameCount, fecScheme);
        }
    }
}

void VoIPController::ProcessExtraData(Buffer &data)
{
    BufferInputStream in(*data, data.Length());
    unsigned char type = in.ReadByte();
    unsigned char fullHash[SHA1_LENGTH];
    crypto.sha1(*data, data.Length(), fullHash);
    uint64_t hash = *reinterpret_cast<uint64_t *>(fullHash);
    if (lastReceivedExtrasByType[type] == hash)
    {
        return;
    }
    LOGE("ProcessExtraData");
    lastReceivedExtrasByType[type] = hash;
    if (type == EXTRA_TYPE_STREAM_FLAGS)
    {
        unsigned char id = in.ReadByte();
        uint32_t flags = static_cast<uint32_t>(in.ReadInt32());
        LOGV("Peer stream state: id %u flags %u", (unsigned int)id, (unsigned int)flags);
        for (shared_ptr<Stream> &s : incomingStreams)
        {
            if (s->id == id)
            {
                bool prevEnabled = s->enabled;
                bool prevPaused = s->paused;
                s->enabled = (flags & STREAM_FLAG_ENABLED) == STREAM_FLAG_ENABLED;
                s->paused = (flags & STREAM_FLAG_PAUSED) == STREAM_FLAG_PAUSED;
                if (flags & STREAM_FLAG_EXTRA_EC)
                {
                    if (!s->extraECEnabled)
                    {
                        s->extraECEnabled = true;
                        if (s->jitterBuffer)
                            s->jitterBuffer->SetMinPacketCount(4);
                    }
                }
                else
                {
                    if (s->extraECEnabled)
                    {
                        s->extraECEnabled = false;
                        if (s->jitterBuffer)
                            s->jitterBuffer->SetMinPacketCount(2);
                    }
                }
                if (prevEnabled != s->enabled && s->type == STREAM_TYPE_VIDEO && videoRenderer)
                    videoRenderer->SetStreamEnabled(s->enabled);
                if (prevPaused != s->paused && s->type == STREAM_TYPE_VIDEO && videoRenderer)
                    videoRenderer->SetStreamPaused(s->paused);
                UpdateAudioOutputState();
                break;
            }
        }
    }
    else if (type == EXTRA_TYPE_STREAM_CSD)
    {
        LOGI("Received codec specific data");
        /*
        os.WriteByte(stream.id);
        os.WriteByte(static_cast<unsigned char>(stream.codecSpecificData.size()));
        for(Buffer& b:stream.codecSpecificData){
            assert(b.Length()<255);
            os.WriteByte(static_cast<unsigned char>(b.Length()));
            os.WriteBytes(b);
        }
        Buffer buf(move(os));
        SendExtra(buf, EXTRA_TYPE_STREAM_CSD);
         */
        unsigned char streamID = in.ReadByte();
        for (shared_ptr<Stream> &stm : incomingStreams)
        {
            if (stm->id == streamID)
            {
                stm->codecSpecificData.clear();
                stm->csdIsValid = false;
                stm->width = static_cast<unsigned int>(in.ReadInt16());
                stm->height = static_cast<unsigned int>(in.ReadInt16());
                size_t count = (size_t)in.ReadByte();
                for (size_t i = 0; i < count; i++)
                {
                    size_t len = (size_t)in.ReadByte();
                    Buffer csd(len);
                    in.ReadBytes(*csd, len);
                    stm->codecSpecificData.push_back(move(csd));
                }
                break;
            }
        }
    }
    else if (type == EXTRA_TYPE_LAN_ENDPOINT)
    {
        if (!allowP2p)
            return;
        LOGV("received lan endpoint (extra)");
        uint32_t peerAddr = in.ReadUInt32();
        uint16_t peerPort = (uint16_t)in.ReadInt32();
        constexpr int64_t lanID = static_cast<int64_t>(FOURCC('L', 'A', 'N', '4')) << 32;
        if (currentEndpoint == lanID)
            currentEndpoint = preferredRelay;

        unsigned char peerTag[16];
        Endpoint lan(lanID, peerPort, NetworkAddress::IPv4(peerAddr), NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_LAN, peerTag);
        MutexGuard m(endpointsMutex);
        endpoints[lanID] = lan;
    }
    else if (type == EXTRA_TYPE_NETWORK_CHANGED)
    {
        LOGI("Peer network changed");
        wasNetworkHandover = true;
        const Endpoint &_currentEndpoint = endpoints.at(currentEndpoint);
        if (_currentEndpoint.type != Endpoint::Type::UDP_RELAY && _currentEndpoint.type != Endpoint::Type::TCP_RELAY)
            currentEndpoint = preferredRelay;
        if (allowP2p)
            SendPublicEndpointsRequest();
        uint32_t flags = in.ReadUInt32();
        dataSavingRequestedByPeer = (flags & INIT_FLAG_DATA_SAVING_ENABLED) == INIT_FLAG_DATA_SAVING_ENABLED;
        UpdateDataSavingState();
        UpdateAudioBitrateLimit();
        ResetEndpointPingStats();
    }
    else if (type == EXTRA_TYPE_GROUP_CALL_KEY)
    {
        if (!didReceiveGroupCallKey && !didSendGroupCallKey)
        {
            unsigned char groupKey[256];
            in.ReadBytes(groupKey, 256);
            messageThread.Post([this, &groupKey] {
                if (callbacks.groupCallKeyReceived)
                    callbacks.groupCallKeyReceived(this, groupKey);
            });
            didReceiveGroupCallKey = true;
        }
    }
    else if (type == EXTRA_TYPE_REQUEST_GROUP)
    {
        if (!didInvokeUpgradeCallback)
        {
            messageThread.Post([this] {
                if (callbacks.upgradeToGroupCallRequested)
                    callbacks.upgradeToGroupCallRequested(this);
            });
            didInvokeUpgradeCallback = true;
        }
    }
    else if (type == EXTRA_TYPE_IPV6_ENDPOINT)
    {
        if (!allowP2p)
            return;
        unsigned char _addr[16];
        in.ReadBytes(_addr, 16);
        NetworkAddress addr = NetworkAddress::IPv6(_addr);
        uint16_t port = static_cast<uint16_t>(in.ReadInt16());
        peerIPv6Available = true;
        LOGV("Received peer IPv6 endpoint [%s]:%u", addr.ToString().c_str(), port);

        constexpr int64_t p2pID = static_cast<int64_t>(FOURCC('P', '2', 'P', '6')) << 32;

        Endpoint ep;
        ep.type = Endpoint::Type::UDP_P2P_INET;
        ep.port = port;
        ep.v6address = addr;
        ep.id = p2pID;
        endpoints[p2pID] = ep;
        if (!myIPv6.IsEmpty())
            currentEndpoint = p2pID;
    }
}

void VoIPController::ProcessAcknowledgedOutgoingExtra(UnacknowledgedExtraData &extra)
{
    if (extra.type == EXTRA_TYPE_GROUP_CALL_KEY)
    {
        if (!didReceiveGroupCallKeyAck)
        {
            didReceiveGroupCallKeyAck = true;
            messageThread.Post([this] {
                if (callbacks.groupCallKeySent)
                    callbacks.groupCallKeySent(this);
            });
        }
    }
}

Endpoint &VoIPController::GetRemoteEndpoint()
{
    return endpoints.at(currentEndpoint);
}

Endpoint *VoIPController::GetEndpointForPacket(const PendingOutgoingPacket &pkt)
{
    Endpoint *endpoint = nullptr;
    if (pkt.endpoint)
    {
        try
        {
            endpoint = &endpoints.at(pkt.endpoint);
        }
        catch (out_of_range &x)
        {
            LOGW("Unable to send packet via nonexistent endpoint %" PRIu64, pkt.endpoint);
            return NULL;
        }
    }
    if (!endpoint)
        endpoint = &endpoints.at(currentEndpoint);
    return endpoint;
}

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