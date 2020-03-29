#include "../../VoIPController.h"
#include "../audio/AudioPacketSender.h"

using namespace tgvoip;


void VoIPController::SetState(int state)
{
    this->state = state;
    LOGV("Call state changed to %d", state);
    stateChangeTime = GetCurrentTime();
    messageThread.Post([this, state] {
        if (callbacks.connectionStateChanged)
            callbacks.connectionStateChanged(this, state);
    });
    if (state == STATE_ESTABLISHED)
    {
        SetMicMute(micMuted);
        if (!wasEstablished)
        {
            wasEstablished = true;
            messageThread.Post(std::bind(&VoIPController::UpdateRTT, this), 0.1, 0.5);
            messageThread.Post(std::bind(&VoIPController::UpdateAudioBitrate, this), 0.0, 0.3);
            messageThread.Post(std::bind(&VoIPController::UpdateCongestion, this), 0.0, 1.0);
            messageThread.Post(std::bind(&VoIPController::UpdateSignalBars, this), 1.0, 1.0);
            messageThread.Post(std::bind(&VoIPController::TickJitterBufferAndCongestionControl, this), 0.0, 0.1);
        }
    }
}

#pragma mark - Timer methods

void VoIPController::SendUdpPings()
{
    LOGW("Send udp pings");
    ENFORCE_MSG_THREAD;

    for (pair<const int64_t, Endpoint> &e : endpoints)
    {
        if (e.second.type == Endpoint::Type::UDP_RELAY)
        {
            SendUdpPing(e.second);
        }
    }
    if (udpConnectivityState == UDP_UNKNOWN || udpConnectivityState == UDP_PING_PENDING)
        udpConnectivityState = UDP_PING_SENT;
    udpPingCount++;
    if (udpPingCount == 4 || udpPingCount == 10)
    {
        messageThread.CancelSelf();
        udpPingTimeoutID = messageThread.Post(std::bind(&VoIPController::EvaluateUdpPingResults, this), 1.0);
    }
}

void VoIPController::EvaluateUdpPingResults()
{
    double avgPongs = 0;
    int count = 0;
    for (pair<const int64_t, Endpoint> &_e : endpoints)
    {
        Endpoint &e = _e.second;
        if (e.type == Endpoint::Type::UDP_RELAY)
        {
            if (e.udpPongCount > 0)
            {
                avgPongs += (double)e.udpPongCount;
                count++;
            }
        }
    }
    if (count > 0)
        avgPongs /= (double)count;
    else
        avgPongs = 0.0;
    LOGI("UDP ping reply count: %.2f", avgPongs);
    if (avgPongs == 0.0 && proxyProtocol == PROXY_SOCKS5 && udpSocket != realUdpSocket)
    {
        LOGI("Proxy does not let UDP through, closing proxy connection and using UDP directly");
        std::shared_ptr<NetworkSocket> proxySocket = udpSocket;
        proxySocket->Close();
        udpSocket = realUdpSocket;
        selectCanceller->CancelSelect();
        proxySupportsUDP = false;
        ResetUdpAvailability();
        return;
    }
    bool configUseTCP = ServerConfig::GetSharedInstance()->GetBoolean("use_tcp", true);
    if (configUseTCP)
    {
        if (avgPongs == 0.0 || (udpConnectivityState == UDP_BAD && avgPongs < 7.0))
        {
            if (needRateFlags & NEED_RATE_FLAG_UDP_NA)
                needRate = true;
            udpConnectivityState = UDP_NOT_AVAILABLE;
            useTCP = true;
            useUDP = avgPongs > 1.0;
            if (endpoints.at(currentEndpoint).type != Endpoint::Type::TCP_RELAY)
                setCurrentEndpointToTCP = true;
            AddTCPRelays();
            waitingForRelayPeerInfo = false;
        }
        else if (avgPongs < 3.0)
        {
            if (needRateFlags & NEED_RATE_FLAG_UDP_BAD)
                needRate = true;
            udpConnectivityState = UDP_BAD;
            useTCP = true;
            setCurrentEndpointToTCP = true;
            AddTCPRelays();
            udpPingTimeoutID = messageThread.Post(std::bind(&VoIPController::SendUdpPings, this), 0.5, 0.5);
        }
        else
        {
            udpPingTimeoutID = MessageThread::INVALID_ID;
            udpConnectivityState = UDP_AVAILABLE;
        }
    }
    else
    {
        udpPingTimeoutID = MessageThread::INVALID_ID;
        udpConnectivityState = UDP_NOT_AVAILABLE;
    }
}
void VoIPController::TickJitterBufferAndCongestionControl()
{
    // TODO get rid of this and update states of these things internally and retroactively
    for (auto &stm : incomingStreams)
    {
        if (stm->type == StreamType::Audio)
        {
            auto astm = dynamic_pointer_cast<IncomingAudioStream>(stm);
            if (astm->jitterBuffer)
                astm->jitterBuffer->Tick();
        }
    }
    conctl.Tick();

    //MutexGuard m(queuedPacketsMutex);
    double currentTime = GetCurrentTime();
    double rtt = GetAverageRTT();
    double packetLossTimeout = std::max(rtt * 2.0, 0.1);
    for (auto &stm : outgoingStreams)
    {
        for (RecentOutgoingPacket &pkt : stm->packetManager.getRecentOutgoingPackets())
        {
            if (pkt.ackTime || pkt.lost)
                continue;
            if (currentTime - pkt.sendTime > packetLossTimeout)
            {
                pkt.lost = true;
                sendLosses++;
                LOGW("Outgoing packet lost: seq=%u, streamId=%hhu, size=%u", pkt.pkt.seq, pkt.pkt.streamId, (unsigned int)pkt.size);

                conctl.PacketLost(pkt.pkt);
                stm->packetSender->PacketLost(pkt);
            }
        }
    }
}

void VoIPController::UpdateRTT()
{
    rttHistory.Add(GetAverageRTT());
    if (rttHistory[0] > 10.0 && rttHistory[8] > 10.0 && (networkType == NET_TYPE_EDGE || networkType == NET_TYPE_GPRS))
    {
        waitingForAcks = true;
    }
    else
    {
        waitingForAcks = false;
    }
    LOGI("RTT=%lf", rttHistory[0])
    //LOGI("%.3lf/%.3lf, rtt diff %.3lf, waiting=%d, queue=%d", rttHistory[0], rttHistory[8], v, waitingForAcks, sendQueue->Size());
    for (auto &stm : incomingStreams)
    {
        if (stm->type == StreamType::Audio)
        {
            auto astm = dynamic_pointer_cast<IncomingAudioStream>(stm);
            if (astm->jitterBuffer)
            {
                int lostCount = astm->jitterBuffer->GetAndResetLostPacketCount();
                if (lostCount > 0 || (lostCount < 0 && recvLossCount > ((uint32_t)-lostCount)))
                    recvLossCount += lostCount;
            }
        }
    }
}

void VoIPController::UpdateCongestion()
{
    if (encoder)
    {
        uint32_t sendLossCount = conctl.GetSendLossCount();
        sendLossCountHistory.Add(sendLossCount - prevSendLossCount);
        prevSendLossCount = sendLossCount;

        uint32_t lastSentSeq = getBestPacketManager().getLastSentSeq();
        packetCountHistory.Add(lastSentSeq - prevSeq);
        prevSeq = lastSentSeq;

        //double packetsPerSec = 1000 / (double)outgoingStreams[0]->frameDuration;
        double avgSendLossCount = sendLossCountHistory.Average() / packetCountHistory.Average();
        LOGE("avg send loss: %.3f%%", avgSendLossCount * 100);

        auto *s = GetStreamByType<OutgoingAudioStream>();
        auto *sender = dynamic_cast<AudioPacketSender *>(s->packetSender.get());
        //avgSendLossCount = sender->setPacketLoss(avgSendLossCount * 100.0) / 100.0;
        sender->setPacketLoss(avgSendLossCount * 100.0);
        if (avgSendLossCount > packetLossToEnableExtraEC && networkType != NET_TYPE_GPRS && networkType != NET_TYPE_EDGE)
        {
            if (!sender->getShittyInternetMode())
            {
                // Shitty Internet Mode™. Redundant redundancy you can trust.
                sender->setShittyInternetMode(true);

                s->extraECEnabled = true;
                SendStreamFlags(*s);

                if (encoder)
                    encoder->SetSecondaryEncoderEnabled(true);
                LOGW("Enabling extra EC");
                if (needRateFlags & NEED_RATE_FLAG_SHITTY_INTERNET_MODE)
                    needRate = true;
                wasExtraEC = true;
            }
        }

        if (avgSendLossCount > 0.08)
        {
            sender->setExtraEcLevel(4);
        }
        else if (avgSendLossCount > 0.05)
        {
            sender->setExtraEcLevel(3);
        }
        else if (avgSendLossCount > 0.02)
        {
            sender->setExtraEcLevel(2);
        }
        else
        {
            sender->setExtraEcLevel(0);
        }
        encoder->SetPacketLoss((int)(avgSendLossCount * 100.0));
        if (avgSendLossCount > rateMaxAcceptableSendLoss)
            needRate = true;

        if ((avgSendLossCount < packetLossToEnableExtraEC || networkType == NET_TYPE_EDGE || networkType == NET_TYPE_GPRS) && sender->getShittyInternetMode())
        {
            sender->setShittyInternetMode(false);

            auto *s = GetStreamByType<OutgoingAudioStream>();
            if (s)
            {
                s->extraECEnabled = false;
                SendStreamFlags(*s);
            }
            if (encoder)
                encoder->SetSecondaryEncoderEnabled(false);
            LOGW("Disabling extra EC");
        }
        if (!wasEncoderLaggy && encoder->GetComplexity() < 10)
            wasEncoderLaggy = true;
    }
}

void VoIPController::UpdateAudioBitrate()
{
    if (encoder)
    {
        double time = GetCurrentTime();
        if ((audioInput && !audioInput->IsInitialized()) || (audioOutput && !audioOutput->IsInitialized()))
        {
            LOGE("Audio I/O failed");
            lastError = ERROR_AUDIO_IO;
            SetState(STATE_FAILED);
        }

        int act = conctl.GetBandwidthControlAction();
        if (dynamic_cast<AudioPacketSender *>(GetStreamByType<OutgoingAudioStream>()->packetSender.get())->getShittyInternetMode())
        {
            //encoder->SetBitrate(8000);
        }
        else if (act == TGVOIP_CONCTL_ACT_DECREASE)
        {
            LOGE("==== DECREASING BITRATE ======");
            uint32_t bitrate = encoder->GetBitrate();
            if (bitrate > 8000)
                encoder->SetBitrate(bitrate < (minAudioBitrate + audioBitrateStepDecr) ? minAudioBitrate : (bitrate - audioBitrateStepDecr));
        }
        else if (act == TGVOIP_CONCTL_ACT_INCREASE)
        {
            uint32_t bitrate = encoder->GetBitrate();
            if (bitrate < maxBitrate)
                encoder->SetBitrate(bitrate + audioBitrateStepIncr);
        }

        if (state == STATE_ESTABLISHED && time - lastRecvPacketTime >= reconnectingTimeout)
        {
            SetState(STATE_RECONNECTING);
            if (needRateFlags & NEED_RATE_FLAG_RECONNECTING)
                needRate = true;
            wasReconnecting = true;
            ResetUdpAvailability();
        }

        if (state == STATE_ESTABLISHED || state == STATE_RECONNECTING)
        {
            if (time - lastRecvPacketTime >= config.recvTimeout)
            {
                const Endpoint &_currentEndpoint = endpoints.at(currentEndpoint);
                if (_currentEndpoint.type != Endpoint::Type::UDP_RELAY && _currentEndpoint.type != Endpoint::Type::TCP_RELAY)
                {
                    LOGW("Packet receive timeout, switching to relay");
                    currentEndpoint = preferredRelay;
                    for (pair<const int64_t, Endpoint> &_e : endpoints)
                    {
                        Endpoint &e = _e.second;
                        if (e.IsP2P())
                        {
                            e.averageRTT = 0;
                            e.rtts.Reset();
                        }
                    }
                    if (allowP2p)
                    {
                        SendPublicEndpointsRequest();
                    }
                    UpdateDataSavingState();
                    UpdateAudioBitrateLimit();
                    SendDataSavingMode();
                    lastRecvPacketTime = time;
                }
                else
                {
                    LOGW("Packet receive timeout, disconnecting");
                    lastError = ERROR_TIMEOUT;
                    SetState(STATE_FAILED);
                }
            }
        }
    }
}

void VoIPController::UpdateSignalBars()
{
    int prevSignalBarCount = GetSignalBarsCount();
    double packetsPerSec = 1000 / (double)GetStreamByID<OutgoingAudioStream>(StreamId::Audio)->frameDuration;
    double avgSendLossCount = sendLossCountHistory.Average() / packetsPerSec;

    int signalBarCount = 4;
    if (state == STATE_RECONNECTING || waitingForAcks)
        signalBarCount = 1;
    if (endpoints.at(currentEndpoint).type == Endpoint::Type::TCP_RELAY)
    {
        signalBarCount = std::min(signalBarCount, 3);
    }
    if (avgSendLossCount > 0.1)
    {
        signalBarCount = 1;
    }
    else if (avgSendLossCount > 0.0625)
    {
        signalBarCount = std::min(signalBarCount, 2);
    }
    else if (avgSendLossCount > 0.025)
    {
        signalBarCount = std::min(signalBarCount, 3);
    }
    for (auto &stm : incomingStreams)
    {
        if (stm->type == StreamType::Audio)
        {
            auto astm = dynamic_pointer_cast<IncomingAudioStream>(stm);
            if (astm->jitterBuffer)
            {
                double avgLateCount[3];
                astm->jitterBuffer->GetAverageLateCount(avgLateCount);
                if (avgLateCount[2] >= 0.2)
                    signalBarCount = 1;
                else if (avgLateCount[2] >= 0.1)
                    signalBarCount = std::min(signalBarCount, 2);
            }
        }
    }

    signalBarsHistory.Add(static_cast<unsigned char>(signalBarCount));
    //LOGV("Signal bar count history %08X", *reinterpret_cast<uint32_t *>(&signalBarsHistory));
    int _signalBarCount = GetSignalBarsCount();
    if (_signalBarCount != prevSignalBarCount)
    {
        LOGD("SIGNAL BAR COUNT CHANGED: %d", _signalBarCount);
        if (callbacks.signalBarCountChanged)
            callbacks.signalBarCountChanged(this, _signalBarCount);
    }
}
