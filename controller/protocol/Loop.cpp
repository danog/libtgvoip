#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

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
            if (packet.data->IsEmpty())
            {
                LOGE("Packet has zero length.");
                continue;
            }
            //LOGV("Received %d bytes from %s:%d at %.5lf", len, packet.address->ToString().c_str(), packet.port, GetCurrentTime());
            messageThread.Post(bind(&VoIPController::NetworkPacketReceived, this, make_shared<NetworkPacket>(move(packet))));
        }

        if (!writeSockets.empty())
        {
            messageThread.Post(bind(&VoIPController::TrySendOutgoingPackets, this));
        }
    }
    LOGI("=== recv thread exiting ===");
}

void VoIPController::RunSendThread()
{
    InitializeAudio();
    InitializeTimers();
    messageThread.Post(bind(&VoIPController::SendInit, this));

    while (true)
    {
        RawPendingOutgoingPacket pkt = rawSendQueue.GetBlocking();
        if (pkt.packet.IsEmpty())
            break;

        if (IS_MOBILE_NETWORK(networkType))
            stats.bytesSentMobile += static_cast<uint64_t>(pkt.packet.data->Length());
        else
            stats.bytesSentWifi += static_cast<uint64_t>(pkt.packet.data->Length());

        if (pkt.packet.protocol == NetworkProtocol::TCP)
        {
            if (pkt.socket && !pkt.socket->IsFailed())
            {
                pkt.socket->Send(std::move(pkt.packet));
            }
        }
        else
        {
            udpSocket->Send(std::move(pkt.packet));
        }
    }

    LOGI("=== send thread exiting ===");
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
            if (e.address == packet.address && e.port == packet.port && CHECK_ENDPOINT_PROTOCOL(e.type, packet.protocol))
            {
                srcEndpointID = e.id;
                break;
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

    if (IS_MOBILE_NETWORK(networkType))
        stats.bytesRecvdMobile += (uint64_t)packet.data->Length();
    else
        stats.bytesRecvdWifi += (uint64_t)packet.data->Length();

    /*try
    {*/
    ProcessIncomingPacket(packet, endpoints.at(srcEndpointID));
    /*}
    catch (out_of_range &x)
    {
        LOGW("Error parsing packet: %s", x.what());
    }*/
}
