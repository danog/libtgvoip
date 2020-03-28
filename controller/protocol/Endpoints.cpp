#include "../../VoIPController.h"

using namespace tgvoip;


Endpoint &VoIPController::GetRemoteEndpoint()
{
    return endpoints.at(currentEndpoint);
}

Endpoint *VoIPController::GetEndpointForPacket(const OutgoingPacket &pkt)
{
    return GetEndpointById(pkt.endpoint);
}


Endpoint *VoIPController::GetEndpointForPacket(const PendingOutgoingPacket &pkt)
{
    return GetEndpointById(pkt.endpoint);
}

Endpoint *VoIPController::GetEndpointById(const int64_t id)
{
    if (id)
    {
        try
        {
            return &endpoints.at(id);
        }
        catch (out_of_range &x)
        {
            LOGW("Unable to send packet via nonexistent endpoint %" PRIu64, id);
            return NULL;
        }
    }
    return &endpoints.at(currentEndpoint);
}

int64_t VoIPController::GetPreferredRelayID()
{
    return preferredRelay;
}

void VoIPController::SetRemoteEndpoints(vector<Endpoint> endpoints, bool allowP2p, int32_t connectionMaxLayer)
{
    LOGW("Set remote endpoints, allowP2P=%d, connectionMaxLayer=%u", allowP2p ? 1 : 0, connectionMaxLayer);
    assert(!runReceiver);
    preferredRelay = 0;

    this->endpoints.clear();
    didAddTcpRelays = false;
    useTCP = true;
    for (auto it = endpoints.begin(); it != endpoints.end(); ++it)
    {
        if (this->endpoints.find(it->id) != this->endpoints.end())
            LOGE("Endpoint IDs are not unique!");
        this->endpoints[it->id] = *it;
        if (currentEndpoint == 0)
            currentEndpoint = it->id;

        if (it->type == Endpoint::Type::UDP_RELAY)
            useTCP = false;
        else if (it->type == Endpoint::Type::TCP_RELAY)
            didAddTcpRelays = true;

        LOGV("Adding endpoint: %s:%d, %s", it->address.ToString().c_str(), it->port, it->type == Endpoint::Type::UDP_RELAY ? "UDP" : "TCP");
    }
    preferredRelay = currentEndpoint;
    this->allowP2p = allowP2p;
    ver.connectionMaxLayer = connectionMaxLayer;
    if (connectionMaxLayer >= 74)
    {
        useMTProto2 = true;
    }
    AddIPv6Relays();
}

void VoIPController::AddIPv6Relays()
{
    if (!myIPv6.IsEmpty() && !didAddIPv6Relays)
    {
        unordered_map<string, vector<Endpoint>> endpointsByAddress;
        for (pair<const int64_t, Endpoint> &_e : endpoints)
        {
            Endpoint &e = _e.second;
            if (e.IsReflector() && !e.v6address.IsEmpty() && !e.address.IsEmpty())
            {
                endpointsByAddress[e.v6address.ToString()].push_back(e);
            }
        }
        MutexGuard m(endpointsMutex);
        for (pair<const string, vector<Endpoint>> &addr : endpointsByAddress)
        {
            for (Endpoint &e : addr.second)
            {
                didAddIPv6Relays = true;
                e.address = NetworkAddress::Empty();
                e.id = e.id ^ (static_cast<int64_t>(FOURCC('I', 'P', 'v', '6')) << 32);
                e.averageRTT = 0;
                e.lastPingSeq = 0;
                e.lastPingTime = 0;
                e.rtts.Reset();
                e.udpPongCount = 0;
                endpoints[e.id] = e;
                LOGD("Adding IPv6-only endpoint [%s]:%u", e.v6address.ToString().c_str(), e.port);
            }
        }
    }
}

void VoIPController::AddTCPRelays()
{
    if (!didAddTcpRelays)
    {
        bool wasSetCurrentToTCP = setCurrentEndpointToTCP;
        LOGV("Adding TCP relays");
        vector<Endpoint> relays;
        for (pair<const int64_t, Endpoint> &_e : endpoints)
        {
            Endpoint &e = _e.second;
            if (e.type != Endpoint::Type::UDP_RELAY)
                continue;
            if (wasSetCurrentToTCP && !useUDP)
            {
                e.rtts.Reset();
                e.averageRTT = 0;
                e.lastPingSeq = 0;
            }
            Endpoint tcpRelay(e);
            tcpRelay.type = Endpoint::Type::TCP_RELAY;
            tcpRelay.averageRTT = 0;
            tcpRelay.lastPingSeq = 0;
            tcpRelay.lastPingTime = 0;
            tcpRelay.rtts.Reset();
            tcpRelay.udpPongCount = 0;
            tcpRelay.id = tcpRelay.id ^ (static_cast<int64_t>(FOURCC('T', 'C', 'P', 0)) << 32);
            if (setCurrentEndpointToTCP && endpoints.at(currentEndpoint).type != Endpoint::Type::TCP_RELAY)
            {
                LOGV("Setting current endpoint to TCP");
                setCurrentEndpointToTCP = false;
                currentEndpoint = tcpRelay.id;
                preferredRelay = tcpRelay.id;
            }
            relays.push_back(tcpRelay);
        }
        MutexGuard m(endpointsMutex);
        for (Endpoint &e : relays)
        {
            endpoints[e.id] = e;
        }
        didAddTcpRelays = true;
    }
}
