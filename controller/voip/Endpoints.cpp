#include "../../VoIPController.h"

using namespace tgvoip;
using namespace std;

shared_ptr<VoIPController::Stream> VoIPController::GetStreamByType(int type, bool outgoing)
{
    shared_ptr<Stream> s;
    for (shared_ptr<Stream> &ss : (outgoing ? outgoingStreams : incomingStreams))
    {
        if (ss->type == type)
            return ss;
    }
    return s;
}

shared_ptr<VoIPController::Stream> VoIPController::GetStreamByID(unsigned char id, bool outgoing)
{
    shared_ptr<Stream> s;
    for (shared_ptr<Stream> &ss : (outgoing ? outgoingStreams : incomingStreams))
    {
        if (ss->id == id)
            return ss;
    }
    return s;
}

CellularCarrierInfo VoIPController::GetCarrierInfo()
{
#if defined(__APPLE__) && TARGET_OS_IOS
    return DarwinSpecific::GetCarrierInfo();
#elif defined(__ANDROID__)
    CellularCarrierInfo carrier;
    jni::DoWithJNI([&carrier](JNIEnv *env) {
        jmethodID getCarrierInfoMethod = env->GetStaticMethodID(jniUtilitiesClass, "getCarrierInfo", "()[Ljava/lang/String;");
        jobjectArray jinfo = (jobjectArray)env->CallStaticObjectMethod(jniUtilitiesClass, getCarrierInfoMethod);
        if (jinfo && env->GetArrayLength(jinfo) == 4)
        {
            carrier.name = jni::JavaStringToStdString(env, (jstring)env->GetObjectArrayElement(jinfo, 0));
            carrier.countryCode = jni::JavaStringToStdString(env, (jstring)env->GetObjectArrayElement(jinfo, 1));
            carrier.mcc = jni::JavaStringToStdString(env, (jstring)env->GetObjectArrayElement(jinfo, 2));
            carrier.mnc = jni::JavaStringToStdString(env, (jstring)env->GetObjectArrayElement(jinfo, 3));
        }
        else
        {
            LOGW("Failed to get carrier info");
        }
    });
    return carrier;
#else
    return CellularCarrierInfo();
#endif
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
