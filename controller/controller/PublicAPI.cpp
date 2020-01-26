#include "../PrivateDefines.cpp"


using namespace tgvoip;
using namespace std;

extern FILE *tgvoipLogFile;

#pragma mark - Public API

VoIPController::VoIPController() : ecAudioPackets(4),
                                   rawSendQueue(64)
{
    selectCanceller = SocketSelectCanceller::Create();
    udpSocket = NetworkSocket::Create(NetworkProtocol::UDP);
    realUdpSocket = udpSocket;

    maxAudioBitrate = ServerConfig::GetSharedInstance()->GetUInt("audio_max_bitrate", 20000);
    maxAudioBitrateGPRS = ServerConfig::GetSharedInstance()->GetUInt("audio_max_bitrate_gprs", 8000);
    maxAudioBitrateEDGE = ServerConfig::GetSharedInstance()->GetUInt("audio_max_bitrate_edge", 16000);
    maxAudioBitrateSaving = ServerConfig::GetSharedInstance()->GetUInt("audio_max_bitrate_saving", 8000);
    initAudioBitrate = ServerConfig::GetSharedInstance()->GetUInt("audio_init_bitrate", 16000);
    initAudioBitrateGPRS = ServerConfig::GetSharedInstance()->GetUInt("audio_init_bitrate_gprs", 8000);
    initAudioBitrateEDGE = ServerConfig::GetSharedInstance()->GetUInt("audio_init_bitrate_edge", 8000);
    initAudioBitrateSaving = ServerConfig::GetSharedInstance()->GetUInt("audio_init_bitrate_saving", 8000);
    audioBitrateStepIncr = ServerConfig::GetSharedInstance()->GetUInt("audio_bitrate_step_incr", 1000);
    audioBitrateStepDecr = ServerConfig::GetSharedInstance()->GetUInt("audio_bitrate_step_decr", 1000);
    minAudioBitrate = ServerConfig::GetSharedInstance()->GetUInt("audio_min_bitrate", 8000);
    relaySwitchThreshold = ServerConfig::GetSharedInstance()->GetDouble("relay_switch_threshold", 0.8);
    p2pToRelaySwitchThreshold = ServerConfig::GetSharedInstance()->GetDouble("p2p_to_relay_switch_threshold", 0.6);
    relayToP2pSwitchThreshold = ServerConfig::GetSharedInstance()->GetDouble("relay_to_p2p_switch_threshold", 0.8);
    reconnectingTimeout = ServerConfig::GetSharedInstance()->GetDouble("reconnecting_state_timeout", 2.0);
    needRateFlags = ServerConfig::GetSharedInstance()->GetUInt("rate_flags", 0xFFFFFFFF);
    rateMaxAcceptableRTT = ServerConfig::GetSharedInstance()->GetDouble("rate_min_rtt", 0.6);
    rateMaxAcceptableSendLoss = ServerConfig::GetSharedInstance()->GetDouble("rate_min_send_loss", 0.2);
    packetLossToEnableExtraEC = ServerConfig::GetSharedInstance()->GetDouble("packet_loss_for_extra_ec", 0.02);
    maxUnsentStreamPackets = ServerConfig::GetSharedInstance()->GetUInt("max_unsent_stream_packets", 2);
    unackNopThreshold = ServerConfig::GetSharedInstance()->GetUInt("unack_nop_threshold", 10);

    shared_ptr<Stream> stm = make_shared<Stream>();
    stm->id = 1;
    stm->type = STREAM_TYPE_AUDIO;
    stm->codec = CODEC_OPUS;
    stm->enabled = 1;
    stm->frameDuration = 60;
    outgoingStreams.push_back(stm);

    recentOutgoingPackets.reserve(MAX_RECENT_PACKETS);
}

VoIPController::~VoIPController()
{
    LOGD("Entered VoIPController::~VoIPController");
    if (!stopping)
    {
        LOGE("!!!!!!!!!!!!!!!!!!!! CALL controller->Stop() BEFORE DELETING THE CONTROLLER OBJECT !!!!!!!!!!!!!!!!!!!!!!!1");
        abort();
    }

    for (auto _stm = incomingStreams.begin(); _stm != incomingStreams.end(); ++_stm)
    {
        shared_ptr<Stream> stm = *_stm;
        LOGD("before stop decoder");
        if (stm->decoder)
        {
            stm->decoder->Stop();
        }
    }
    LOGD("before delete encoder");
    if (encoder)
    {
        encoder->Stop();
    }
    LOGD("before delete echo canceller");
    if (echoCanceller)
    {
        echoCanceller->Stop();
    }
    LOGD("Left VoIPController::~VoIPController");
    if (tgvoipLogFile)
    {
        FILE *log = tgvoipLogFile;
        tgvoipLogFile = nullptr;
        fclose(log);
    }
}

void VoIPController::Stop()
{
    LOGD("Entered VoIPController::Stop");
    stopping = true;
    runReceiver = false;
    LOGD("before shutdown socket");
    if (udpSocket)
        udpSocket->Close();
    if (realUdpSocket != udpSocket)
        realUdpSocket->Close();
    selectCanceller->CancelSelect();
    //Buffer emptyBuf(0);
    //PendingOutgoingPacket emptyPacket{0, 0, 0, move(emptyBuf), 0};
    //sendQueue->Put(move(emptyPacket));
    rawSendQueue.Put(RawPendingOutgoingPacket{NetworkPacket::Empty(), nullptr});
    LOGD("before join sendThread");
    if (sendThread)
    {
        sendThread->Join();
    }
    LOGD("before join recvThread");
    if (recvThread)
    {
        recvThread->Join();
    }
    LOGD("before stop messageThread");
    messageThread.Stop();
    {
        LOGD("Before stop audio I/O");
        MutexGuard m(audioIOMutex);
        if (audioInput)
        {
            audioInput->Stop();
            audioInput->SetCallback(NULL, NULL);
        }
        if (audioOutput)
        {
            audioOutput->Stop();
            audioOutput->SetCallback(NULL, NULL);
        }
    }
    LOGD("Left VoIPController::Stop [need rate = %d]", (int)needRate);
}

void VoIPController::Start()
{
    LOGW("Starting voip controller");
    udpSocket->Open();
    if (udpSocket->IsFailed())
    {
        SetState(STATE_FAILED);
        return;
    }

    runReceiver = true;
    recvThread.reset(new Thread(bind(&VoIPController::RunRecvThread, this)));
    recvThread->SetName("VoipRecv");
    recvThread->Start();

    messageThread.Start();
}

void VoIPController::Connect()
{
    assert(state != STATE_WAIT_INIT_ACK);
    connectionInitTime = GetCurrentTime();
    if (config.initTimeout == 0.0)
    {
        LOGE("Init timeout is 0 -- did you forget to set config?");
        config.initTimeout = 30.0;
    }

    //InitializeTimers();
    //SendInit();
    sendThread.reset(new Thread(bind(&VoIPController::RunSendThread, this)));
    sendThread->SetName("VoipSend");
    sendThread->Start();
}

bool VoIPController::NeedRate()
{
    return needRate && ServerConfig::GetSharedInstance()->GetBoolean("bad_call_rating", false);
}

void VoIPController::SetMicMute(bool mute)
{
    if (micMuted == mute)
        return;
    micMuted = mute;
    if (audioInput)
    {
        if (mute)
            audioInput->Stop();
        else
            audioInput->Start();
        if (!audioInput->IsInitialized())
        {
            lastError = ERROR_AUDIO_IO;
            SetState(STATE_FAILED);
            return;
        }
    }
    if (echoCanceller)
        echoCanceller->Enable(!mute);
    if (state == STATE_ESTABLISHED)
    {
        messageThread.Post([this] {
            for (shared_ptr<Stream> &s : outgoingStreams)
            {
                if (s->type == STREAM_TYPE_AUDIO)
                {
                    s->enabled = !micMuted;
                    if (peerVersion < 6)
                    {
                        unsigned char buf[2];
                        buf[0] = s->id;
                        buf[1] = (char)(micMuted ? 0 : 1);
                        SendPacketReliably(PKT_STREAM_STATE, buf, 2, .5f, 20);
                    }
                    else
                    {
                        SendStreamFlags(*s);
                    }
                }
            }
        });
    }
}

string VoIPController::GetDebugString()
{
    string r = "Remote endpoints: \n";
    char buffer[2048];
    MutexGuard m(endpointsMutex);
    for (pair<const int64_t, Endpoint> &_e : endpoints)
    {
        Endpoint &endpoint = _e.second;
        const char *type;
        switch (endpoint.type)
        {
        case Endpoint::Type::UDP_P2P_INET:
            type = "UDP_P2P_INET";
            break;
        case Endpoint::Type::UDP_P2P_LAN:
            type = "UDP_P2P_LAN";
            break;
        case Endpoint::Type::UDP_RELAY:
            type = "UDP_RELAY";
            break;
        case Endpoint::Type::TCP_RELAY:
            type = "TCP_RELAY";
            break;
        default:
            type = "UNKNOWN";
            break;
        }
        snprintf(buffer, sizeof(buffer), "%s:%u %dms %d 0x%" PRIx64 " [%s%s]\n", endpoint.address.IsEmpty() ? ("[" + endpoint.v6address.ToString() + "]").c_str() : endpoint.address.ToString().c_str(), endpoint.port, (int)(endpoint.averageRTT * 1000), endpoint.udpPongCount, (uint64_t)endpoint.id, type, currentEndpoint == endpoint.id ? ", IN_USE" : "");
        r += buffer;
    }
    if (shittyInternetMode)
    {
        snprintf(buffer, sizeof(buffer), "ShittyInternetMode: level %u\n", extraEcLevel);
        r += buffer;
    }
    double avgLate[3];
    shared_ptr<Stream> stm = GetStreamByType(STREAM_TYPE_AUDIO, false);
    shared_ptr<JitterBuffer> jitterBuffer;
    if (stm)
        jitterBuffer = stm->jitterBuffer;
    if (jitterBuffer)
        jitterBuffer->GetAverageLateCount(avgLate);
    else
        memset(avgLate, 0, 3 * sizeof(double));
    snprintf(buffer, sizeof(buffer),
             "Jitter buffer: %d/%.2f | %.1f, %.1f, %.1f\n"
             "RTT avg/min: %d/%d\n"
             "Congestion window: %d/%d bytes\n"
             "Key fingerprint: %02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%s\n"
             "Last sent/ack'd seq: %u/%u\n"
             "Last recvd seq: %u\n"
             "Send/recv losses: %u/%u (%d%%)\n"
             "Audio bitrate: %d kbit\n"
             "Outgoing queue: %u\n"
             //					 "Packet grouping: %d\n"
             "Frame size out/in: %d/%d\n"
             "Bytes sent/recvd: %llu/%llu",
             jitterBuffer ? jitterBuffer->GetMinPacketCount() : 0, jitterBuffer ? jitterBuffer->GetAverageDelay() : 0, avgLate[0], avgLate[1], avgLate[2],
             // (int)(GetAverageRTT()*1000), 0,
             (int)(conctl.GetAverageRTT() * 1000), (int)(conctl.GetMinimumRTT() * 1000),
             int(conctl.GetInflightDataSize()), int(conctl.GetCongestionWindow()),
             keyFingerprint[0], keyFingerprint[1], keyFingerprint[2], keyFingerprint[3], keyFingerprint[4], keyFingerprint[5], keyFingerprint[6], keyFingerprint[7],
             useMTProto2 ? " (MTProto2.0)" : "",
             lastSentSeq, lastRemoteAckSeq, lastRemoteSeq,
             sendLosses, recvLossCount, encoder ? encoder->GetPacketLoss() : 0,
             encoder ? (encoder->GetBitrate() / 1000) : 0,
             static_cast<unsigned int>(unsentStreamPackets),
             //			 audioPacketGrouping,
             outgoingStreams[0]->frameDuration, incomingStreams.size() > 0 ? incomingStreams[0]->frameDuration : 0,
             (long long unsigned int)(stats.bytesSentMobile + stats.bytesSentWifi),
             (long long unsigned int)(stats.bytesRecvdMobile + stats.bytesRecvdWifi));
    r += buffer;

    if (config.enableVideoSend)
    {
        shared_ptr<Stream> vstm = GetStreamByType(STREAM_TYPE_VIDEO, true);
        if (vstm && vstm->enabled && videoPacketSender)
        {
            snprintf(buffer, sizeof(buffer), "\nVideo out: %ux%u '%c%c%c%c' %u kbit", vstm->width, vstm->height, PRINT_FOURCC(vstm->codec), videoPacketSender->GetBitrate());
            r += buffer;
        }
    }
    if (!peerVideoDecoders.empty())
    {
        r += "\nPeer codecs: ";
        for (uint32_t codec : peerVideoDecoders)
        {
            snprintf(buffer, sizeof(buffer), "'%c%c%c%c' ", PRINT_FOURCC(codec));
            r += buffer;
        }
    }
    if (config.enableVideoReceive)
    {
        shared_ptr<Stream> vstm = GetStreamByType(STREAM_TYPE_VIDEO, false);
        if (vstm && vstm->enabled)
        {
            snprintf(buffer, sizeof(buffer), "\nVideo in: %ux%u '%c%c%c%c'", vstm->width, vstm->height, PRINT_FOURCC(vstm->codec));
            r += buffer;
        }
    }

    return r;
}


const char *VoIPController::GetVersion()
{
    return LIBTGVOIP_VERSION;
}

int VoIPController::GetLastError()
{
    return lastError;
}

void VoIPController::GetStats(TrafficStats *stats)
{
    memcpy(stats, &this->stats, sizeof(TrafficStats));
}

string VoIPController::GetDebugLog()
{
    map<string, json11::Json> network{
        {"type", NetworkTypeToString(networkType)}};
    if (IS_MOBILE_NETWORK(networkType))
    {
        CellularCarrierInfo carrier = GetCarrierInfo();
        if (!carrier.name.empty())
        {
            network["carrier"] = carrier.name;
            network["country"] = carrier.countryCode;
            network["mcc"] = carrier.mcc;
            network["mnc"] = carrier.mnc;
        }
    }
    else if (networkType == NET_TYPE_WIFI)
    {
#ifdef __ANDROID__
        jni::DoWithJNI([&](JNIEnv *env) {
            jmethodID getWifiInfoMethod = env->GetStaticMethodID(jniUtilitiesClass, "getWifiInfo", "()[I");
            jintArray res = static_cast<jintArray>(env->CallStaticObjectMethod(jniUtilitiesClass, getWifiInfoMethod));
            if (res)
            {
                jint *wifiInfo = env->GetIntArrayElements(res, NULL);
                network["rssi"] = wifiInfo[0];
                network["link_speed"] = wifiInfo[1];
                env->ReleaseIntArrayElements(res, wifiInfo, JNI_ABORT);
            }
        });
#endif
    }
    /*vector<json11::Json> lpkts;
    for(DebugLoggedPacket& lpkt:debugLoggedPackets){
        lpkts.push_back(json11::Json::array{lpkt.timestamp, lpkt.seq, lpkt.length});
    }
    return json11::Json(json11::Json::object{
            {"log_type", "out_packet_stats"},
            {"libtgvoip_version", LIBTGVOIP_VERSION},
            {"network", network},
            {"protocol_version", std::min(peerVersion, PROTOCOL_VERSION)},
            {"total_losses", json11::Json::object{
                    {"s", (int32_t)conctl.GetSendLossCount()},
                    {"r", (int32_t)recvLossCount}
            }},
            {"call_duration", GetCurrentTime()-connectionInitTime},
            {"out_packet_stats", lpkts}
    }).dump();*/

    vector<json11::Json> _endpoints;
    for (pair<const int64_t, Endpoint> &_e : endpoints)
    {
        Endpoint &e = _e.second;
        string type;
        map<string, json11::Json> je{
            {"rtt", (int)(e.averageRTT * 1000.0)}};
        int64_t id = 0;
        if (e.type == Endpoint::Type::UDP_RELAY)
        {
            je["type"] = e.IsIPv6Only() ? "udp_relay6" : "udp_relay";
            id = e.CleanID();
            if (e.totalUdpPings == 0)
                je["udp_pings"] = 0.0;
            else
                je["udp_pings"] = (double)e.totalUdpPingReplies / (double)e.totalUdpPings;
            je["self_rtt"] = (int)(e.selfRtts.Average() * 1000.0);
        }
        else if (e.type == Endpoint::Type::TCP_RELAY)
        {
            je["type"] = e.IsIPv6Only() ? "tcp_relay6" : "tcp_relay";
            id = e.CleanID();
        }
        else if (e.type == Endpoint::Type::UDP_P2P_INET)
        {
            je["type"] = e.IsIPv6Only() ? "p2p_inet6" : "p2p_inet";
        }
        else if (e.type == Endpoint::Type::UDP_P2P_LAN)
        {
            je["type"] = "p2p_lan";
        }
        if (preferredRelay == e.id && wasEstablished)
            je["pref"] = true;
        if (id)
        {
            ostringstream s;
            s << id;
            je["id"] = s.str();
        }
        _endpoints.push_back(je);
    }

    string p2pType = "none";
    Endpoint &cur = endpoints[currentEndpoint];
    if (cur.type == Endpoint::Type::UDP_P2P_INET)
        p2pType = cur.IsIPv6Only() ? "inet6" : "inet";
    else if (cur.type == Endpoint::Type::UDP_P2P_LAN)
        p2pType = "lan";

    vector<string> problems;
    if (lastError == ERROR_TIMEOUT)
        problems.push_back("timeout");
    if (wasReconnecting)
        problems.push_back("reconnecting");
    if (wasExtraEC)
        problems.push_back("extra_ec");
    if (wasEncoderLaggy)
        problems.push_back("encoder_lag");
    if (!wasEstablished)
        problems.push_back("not_inited");
    if (wasNetworkHandover)
        problems.push_back("network_handover");

    return json11::Json(json11::Json::object{
                            {"log_type", "call_stats"},
                            {"libtgvoip_version", LIBTGVOIP_VERSION},
                            {"network", network},
                            {"protocol_version", std::min(peerVersion, PROTOCOL_VERSION)},
                            {"udp_avail", udpConnectivityState == UDP_AVAILABLE},
                            {"tcp_used", useTCP},
                            {"p2p_type", p2pType},
                            {"packet_stats", json11::Json::object{
                                                 {"out", (int)seq},
                                                 {"in", (int)packetsReceived},
                                                 {"lost_out", (int)conctl.GetSendLossCount()},
                                                 {"lost_in", (int)recvLossCount}}},
                            {"endpoints", _endpoints},
                            {"problems", problems}})
        .dump();
}

vector<AudioInputDevice> VoIPController::EnumerateAudioInputs()
{
    vector<AudioInputDevice> devs;
    audio::AudioInput::EnumerateDevices(devs);
    return devs;
}

vector<AudioOutputDevice> VoIPController::EnumerateAudioOutputs()
{
    vector<AudioOutputDevice> devs;
    audio::AudioOutput::EnumerateDevices(devs);
    return devs;
}

void VoIPController::SetCurrentAudioInput(string id)
{
    currentAudioInput = id;
    if (audioInput)
        audioInput->SetCurrentDevice(id);
}

void VoIPController::SetCurrentAudioOutput(string id)
{
    currentAudioOutput = id;
    if (audioOutput)
        audioOutput->SetCurrentDevice(id);
}

string VoIPController::GetCurrentAudioInputID()
{
    return currentAudioInput;
}

string VoIPController::GetCurrentAudioOutputID()
{
    return currentAudioOutput;
}

void VoIPController::SetProxy(int protocol, string address, uint16_t port, string username, string password)
{
    proxyProtocol = protocol;
    proxyAddress = std::move(address);
    proxyPort = port;
    proxyUsername = std::move(username);
    proxyPassword = std::move(password);
}

int VoIPController::GetSignalBarsCount()
{
    return signalBarsHistory.NonZeroAverage();
}

void VoIPController::SetCallbacks(VoIPController::Callbacks callbacks)
{
    this->callbacks = callbacks;
    if (callbacks.connectionStateChanged)
        callbacks.connectionStateChanged(this, state);
}

void VoIPController::SetAudioOutputGainControlEnabled(bool enabled)
{
    LOGD("New output AGC state: %d", enabled);
}

uint32_t VoIPController::GetPeerCapabilities()
{
    return peerCapabilities;
}

void VoIPController::SendGroupCallKey(unsigned char *key)
{
    Buffer buf(256);
    buf.CopyFrom(key, 0, 256);
    shared_ptr<Buffer> keyPtr = make_shared<Buffer>(move(buf));
    messageThread.Post([this, keyPtr] {
        if (!(peerCapabilities & TGVOIP_PEER_CAP_GROUP_CALLS))
        {
            LOGE("Tried to send group call key but peer isn't capable of them");
            return;
        }
        if (didSendGroupCallKey)
        {
            LOGE("Tried to send a group call key repeatedly");
            return;
        }
        if (!isOutgoing)
        {
            LOGE("You aren't supposed to send group call key in an incoming call, use VoIPController::RequestCallUpgrade() instead");
            return;
        }
        didSendGroupCallKey = true;
        SendExtra(*keyPtr, EXTRA_TYPE_GROUP_CALL_KEY);
    });
}

void VoIPController::RequestCallUpgrade()
{
    messageThread.Post([this] {
        if (!(peerCapabilities & TGVOIP_PEER_CAP_GROUP_CALLS))
        {
            LOGE("Tried to send group call key but peer isn't capable of them");
            return;
        }
        if (didSendUpgradeRequest)
        {
            LOGE("Tried to send upgrade request repeatedly");
            return;
        }
        if (isOutgoing)
        {
            LOGE("You aren't supposed to send an upgrade request in an outgoing call, generate an encryption key and use VoIPController::SendGroupCallKey instead");
            return;
        }
        didSendUpgradeRequest = true;
        Buffer empty(0);
        SendExtra(empty, EXTRA_TYPE_REQUEST_GROUP);
    });
}

void VoIPController::SetEchoCancellationStrength(int strength)
{
    echoCancellationStrength = strength;
    if (echoCanceller)
        echoCanceller->SetAECStrength(strength);
}

int VoIPController::GetConnectionState()
{
    return state;
}

void VoIPController::SetConfig(const Config &cfg)
{
    config = cfg;
    if (tgvoipLogFile)
    {
        fclose(tgvoipLogFile);
        tgvoipLogFile = nullptr;
    }
    if (!config.logFilePath.empty())
    {
#ifndef _WIN32
        tgvoipLogFile = fopen(config.logFilePath.c_str(), "a");
#else
        if (_wfopen_s(&tgvoipLogFile, config.logFilePath.c_str(), L"a") != 0)
        {
            tgvoipLogFile = nullptr;
        }
#endif
        tgvoip_log_file_write_header(tgvoipLogFile);
    }
    else
    {
        tgvoipLogFile = nullptr;
    }
    if (!config.statsDumpFilePath.empty())
    {
        statsDump.open(config.statsDumpFilePath);
        if (statsDump)
            statsDump << "Time\tRTT\tLRSeq\tLSSeq\tLASeq\tLostR\tLostS\tCWnd\tBitrate\tLoss%%\tJitter\tJDelay\tAJDelay\n";
    }
    else
    {
        statsDump.close();
    }
    UpdateDataSavingState();
    UpdateAudioBitrateLimit();
}

void VoIPController::SetPersistentState(vector<uint8_t> state)
{
    using namespace json11;

    if (state.empty())
        return;
    string jsonErr;
    string json = string(state.begin(), state.end());
    Json _obj = Json::parse(json, jsonErr);
    if (!jsonErr.empty())
    {
        LOGE("Error parsing persistable state: %s", jsonErr.c_str());
        return;
    }
    Json::object obj = _obj.object_items();
    if (obj.find("proxy") != obj.end())
    {
        Json::object proxy = obj["proxy"].object_items();
        lastTestedProxyServer = proxy["server"].string_value();
        proxySupportsUDP = proxy["udp"].bool_value();
        proxySupportsTCP = proxy["tcp"].bool_value();
    }
}

vector<uint8_t> VoIPController::GetPersistentState()
{
    using namespace json11;

    Json::object obj = Json::object{
        {"ver", 1},
    };
    if (proxyProtocol == PROXY_SOCKS5)
    {
        char pbuf[128];
        snprintf(pbuf, sizeof(pbuf), "%s:%u", proxyAddress.c_str(), proxyPort);
        obj.insert({"proxy", Json::object{
                                 {"server", string(pbuf)},
                                 {"udp", proxySupportsUDP},
                                 {"tcp", proxySupportsTCP}}});
    }
    string _jstr = Json(obj).dump();
    const char *jstr = _jstr.c_str();
    return vector<uint8_t>(jstr, jstr + strlen(jstr));
}


void VoIPController::SetOutputVolume(float level)
{
    outputVolume->SetLevel(level);
}

void VoIPController::SetInputVolume(float level)
{
    inputVolume->SetLevel(level);
}

#if defined(__APPLE__) && TARGET_OS_OSX
void VoIPController::SetAudioOutputDuckingEnabled(bool enabled)
{
    macAudioDuckingEnabled = enabled;
    audio::AudioUnitIO *osxAudio = dynamic_cast<audio::AudioUnitIO *>(audioIO.get());
    if (osxAudio)
    {
        osxAudio->SetDuckingEnabled(enabled);
    }
}
#endif


#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
void VoIPController::SetAudioDataCallbacks(std::function<void(int16_t *, size_t)> input, std::function<void(int16_t *, size_t)> output, std::function<void(int16_t *, size_t)> preproc = nullptr)
{
    audioInputDataCallback = input;
    audioOutputDataCallback = output;
    audioPreprocDataCallback = preproc;
}
#endif


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

