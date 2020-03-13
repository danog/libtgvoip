#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

#pragma mark - Bandwidth management


double VoIPController::GetAverageRTT()
{
    ENFORCE_MSG_THREAD;
    PacketManager &pm = getBestPacketManager();
    if (pm.getLastSentSeq() >= pm.getLastAckedSeq())
    {
        uint32_t diff = pm.getLastSentSeq() - pm.getLastAckedSeq();
        //LOGV("rtt diff=%u", diff);
        if (diff < 32)
        {
            double res = 0;
            size_t count = 0;
            for (const auto &packet : pm.getRecentOutgoingPackets())
            {
                if (packet.rttTime)
                {
                    res += packet.rttTime;
                    count++;
                }
            }
            if (count)
                res /= count;
            return res;
        }
    }
    return 999;
}


void VoIPController::SetNetworkType(int type)
{
    networkType = type;
    UpdateDataSavingState();
    UpdateAudioBitrateLimit();
    myIPv6 = NetworkAddress::Empty();
    string itfName = udpSocket->GetLocalInterfaceInfo(NULL, &myIPv6);
    LOGI("set network type: %s, active interface %s", NetworkTypeToString(type).c_str(), itfName.c_str());
    LOGI("Local IPv6 address: %s", myIPv6.ToString().c_str());
    if (IS_MOBILE_NETWORK(networkType))
    {
        CellularCarrierInfo carrier = GetCarrierInfo();
        if (!carrier.name.empty())
        {
            LOGI("Carrier: %s [%s; mcc=%s, mnc=%s]", carrier.name.c_str(), carrier.countryCode.c_str(), carrier.mcc.c_str(), carrier.mnc.c_str());
        }
    }
    if (itfName != activeNetItfName)
    {
        udpSocket->OnActiveInterfaceChanged();
        LOGI("Active network interface changed: %s -> %s", activeNetItfName.c_str(), itfName.c_str());
        bool isFirstChange = activeNetItfName.length() == 0 && state != STATE_ESTABLISHED && state != STATE_RECONNECTING;
        activeNetItfName = itfName;
        if (isFirstChange)
            return;
        messageThread.Post([this] {
            wasNetworkHandover = true;
            if (currentEndpoint)
            {
                const Endpoint &_currentEndpoint = endpoints.at(currentEndpoint);
                const Endpoint &_preferredRelay = endpoints.at(preferredRelay);
                if (_currentEndpoint.type != Endpoint::Type::UDP_RELAY)
                {
                    if (_preferredRelay.type == Endpoint::Type::UDP_RELAY)
                        currentEndpoint = preferredRelay;
                    MutexGuard m(endpointsMutex);
                    constexpr int64_t lanID = static_cast<int64_t>(FOURCC('L', 'A', 'N', '4')) << 32;
                    endpoints.erase(lanID);
                    for (pair<const int64_t, Endpoint> &e : endpoints)
                    {
                        Endpoint &endpoint = e.second;
                        if (endpoint.type == Endpoint::Type::UDP_RELAY && useTCP)
                        {
                            useTCP = false;
                            if (_preferredRelay.type == Endpoint::Type::TCP_RELAY)
                            {
                                preferredRelay = currentEndpoint = endpoint.id;
                            }
                        }
                        else if (endpoint.type == Endpoint::Type::TCP_RELAY && endpoint.socket)
                        {
                            endpoint.socket->Close();
                        }
                        endpoint.averageRTT = 0;
                        endpoint.rtts.Reset();
                    }
                }
            }
            lastUdpPingTime = 0;
            if (proxyProtocol == PROXY_SOCKS5)
                InitUDPProxy();
            if (allowP2p && currentEndpoint)
            {
                SendPublicEndpointsRequest();
            }
            BufferOutputStream s(4);
            s.WriteInt32(dataSavingMode ? INIT_FLAG_DATA_SAVING_ENABLED : 0);
            if (peerVersion < 6)
            {
                SendPacketReliably(PKT_NETWORK_CHANGED, s.GetBuffer(), s.GetLength(), 1, 20);
            }
            else
            {
                Buffer buf(move(s));
                SendExtra(buf, EXTRA_TYPE_NETWORK_CHANGED);
            }
            needReInitUdpProxy = true;
            selectCanceller->CancelSelect();
            didSendIPv6Endpoint = false;

            AddIPv6Relays();
            ResetUdpAvailability();
            ResetEndpointPingStats();
        });
    }
}


void VoIPController::UpdateAudioBitrateLimit()
{
    if (encoder)
    {
        if (dataSavingMode || dataSavingRequestedByPeer)
        {
            maxBitrate = maxAudioBitrateSaving;
            encoder->SetBitrate(initAudioBitrateSaving);
        }
        else if (networkType == NET_TYPE_GPRS)
        {
            maxBitrate = maxAudioBitrateGPRS;
            encoder->SetBitrate(initAudioBitrateGPRS);
        }
        else if (networkType == NET_TYPE_EDGE)
        {
            maxBitrate = maxAudioBitrateEDGE;
            encoder->SetBitrate(initAudioBitrateEDGE);
        }
        else
        {
            maxBitrate = maxAudioBitrate;
            encoder->SetBitrate(initAudioBitrate);
        }
        encoder->SetVadMode(dataSavingMode || dataSavingRequestedByPeer);
        if (echoCanceller)
            echoCanceller->SetVoiceDetectionEnabled(dataSavingMode || dataSavingRequestedByPeer);
    }
}

void VoIPController::UpdateDataSavingState()
{
    if (config.dataSaving == DATA_SAVING_ALWAYS)
    {
        dataSavingMode = true;
    }
    else if (config.dataSaving == DATA_SAVING_MOBILE)
    {
        dataSavingMode = IS_MOBILE_NETWORK(networkType);
    }
    else
    {
        dataSavingMode = false;
    }
    LOGI("update data saving mode, config %d, enabled %d, reqd by peer %d", config.dataSaving, dataSavingMode, dataSavingRequestedByPeer);
}
