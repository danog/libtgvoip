
#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

bool VoIPController::parseRelayPacket(const BufferInputStream &in, Endpoint &srcEndpoint)
{
    size_t offset = in.GetOffset();
    if (!(in.ReadUInt64() == 0xFFFFFFFFFFFFFFFFLL && in.ReadUInt32() == 0xFFFFFFFF))
    {
        in.Seek(offset);
        return false;
    }

    // UDP relay special request response
    in.Seek(16 + 12);
    uint32_t tlid = in.ReadUInt32();

    if (tlid == TLID_UDP_REFLECTOR_SELF_INFO)
    {
        if (srcEndpoint.type == Endpoint::Type::UDP_RELAY && in.Remaining() >= 32)
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
                        SendExtra(b, ExtraIpv6Endpoint::ID);
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
                        SendExtra(buf, ExtraLanEndpoint::ID);
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
    return true;
}