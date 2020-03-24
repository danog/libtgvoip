//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#pragma once
#include "NetworkSocket.h"
#include <map>
#include <memory>

namespace tgvoip
{
class VoIPGroupController;
class VoIPController;
class Endpoint
{
    friend class VoIPController;
    friend class VoIPGroupController;

public:
    enum Type
    {
        UDP_P2P_INET = 1,
        UDP_P2P_LAN,
        UDP_RELAY,
        TCP_RELAY
    };

    enum ID : int64_t
    {
        P2Pv4 = static_cast<int64_t>(FOURCC('P', '2', 'P', '4')) << 32,
        LANv4 = static_cast<int64_t>(FOURCC('L', 'A', 'N', '4')) << 32,

        P2Pv6 = static_cast<int64_t>(FOURCC('P', '2', 'P', '6')) << 32,

        Any = 0
    };

    Endpoint(int64_t id, uint16_t port, const IPv4Address &address, const IPv6Address &v6address, Type type, unsigned char *peerTag);
    Endpoint(int64_t id, uint16_t port, const NetworkAddress address, const NetworkAddress v6address, Type type, unsigned char *peerTag);
    Endpoint();
    ~Endpoint();
    const NetworkAddress &GetAddress() const;
    NetworkAddress &GetAddress();
    bool IsIPv6Only() const;
    int64_t CleanID() const;
    int64_t id;
    uint16_t port;
    NetworkAddress address;
    NetworkAddress v6address;
    Type type;
    unsigned char peerTag[16];

    inline const bool IsP2P() const
    {
        return type == UDP_P2P_INET || type == UDP_P2P_LAN;
    }
    inline const bool IsReflector() const
    {
        return type == UDP_RELAY || type == TCP_RELAY;
    }

private:
    double lastPingTime;
    uint32_t lastPingSeq;
    HistoricBuffer<double, 6> rtts;
    HistoricBuffer<double, 4> selfRtts;
    std::map<int64_t, double> udpPingTimes;
    double averageRTT;
    std::shared_ptr<NetworkSocket> socket;
    int udpPongCount;
    int totalUdpPings = 0;
    int totalUdpPingReplies = 0;
};
} // namespace tgvoip