
#include "../../VoIPController.h"

using namespace tgvoip;


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
                        auto s = std::make_shared<ExtraIpv6Endpoint>();
                        s->address = realAddr;
                        s->port = udpSocket->GetLocalPort();
                        SendExtra(s);
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

            if (currentEndpoint == Endpoint::ID::P2Pv4 || currentEndpoint == Endpoint::ID::LANv4)
                currentEndpoint = preferredRelay;

            if (endpoints.find(Endpoint::ID::LANv4) != endpoints.end())
            {
                MutexGuard m(endpointsMutex);
                endpoints.erase(Endpoint::ID::LANv4);
            }

            unsigned char peerTag[16];
            LOGW("Received reflector peer info, my=%s:%u, peer=%s:%u", NetworkAddress::IPv4(myAddr).ToString().c_str(), myPort, NetworkAddress::IPv4(peerAddr).ToString().c_str(), peerPort);
            if (waitingForRelayPeerInfo)
            {
                Endpoint p2p(Endpoint::ID::P2Pv4, (uint16_t)peerPort, NetworkAddress::IPv4(peerAddr), NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_INET, peerTag);
                {
                    MutexGuard m(endpointsMutex);
                    endpoints[Endpoint::ID::P2Pv4] = p2p;
                }
                if (myAddr == peerAddr)
                {
                    LOGW("Detected LAN");
                    NetworkAddress lanAddr = NetworkAddress::IPv4(0);
                    udpSocket->GetLocalInterfaceInfo(&lanAddr, NULL);

                    auto extra = std::make_shared<ExtraLanEndpoint>();
                    extra->address = lanAddr;
                    extra->port = udpSocket->GetLocalPort();
                    SendExtra(extra);
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