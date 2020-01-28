#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

#pragma mark - Internal intialization

VoIPController::VoIPController() : rawSendQueue(64)
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
    stm->packetSender = std::make_unique<AudioPacketSender>(this, nullptr, stm);

    outgoingStreams.push_back(stm);

    recentOutgoingPackets.reserve(MAX_RECENT_PACKETS);
}

void VoIPController::InitializeTimers()
{
    initTimeoutID = messageThread.Post(
        [this] {
            LOGW("Init timeout, disconnecting");
            lastError = ERROR_TIMEOUT;
            SetState(STATE_FAILED);
        },
        config.initTimeout);

    if (!config.statsDumpFilePath.empty())
    {
        messageThread.Post(
            [this] {
                if (statsDump && incomingStreams.size() == 1)
                {
                    shared_ptr<JitterBuffer> &jitterBuffer = incomingStreams[0]->jitterBuffer;
                    statsDump << std::setprecision(3)
                              << GetCurrentTime() - connectionInitTime
                              << endpoints.at(currentEndpoint).rtts[0]
                              << lastRemoteSeq
                              << (uint32_t)getLocalSeq()
                              << peerAcks[0]
                              << recvLossCount
                              << conctl.GetSendLossCount()
                              << (int)conctl.GetInflightDataSize()
                              << (encoder ? encoder->GetBitrate() : 0)
                              << (encoder ? encoder->GetPacketLoss() : 0)
                              << (jitterBuffer ? jitterBuffer->GetLastMeasuredJitter() : 0)
                              << (jitterBuffer ? jitterBuffer->GetLastMeasuredDelay() * 0.06 : 0)
                              << (jitterBuffer ? jitterBuffer->GetAverageDelay() * 0.06 : 0);
                }
            },
            0.1, 0.1);
    }

    messageThread.Post(std::bind(&VoIPController::SendRelayPings, this), 0.0, 2.0);
}
