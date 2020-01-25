
#pragma mark - Internal intialization

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
                              << (uint32_t)seq
                              << lastRemoteAckSeq
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
            stats.bytesSentMobile += static_cast<uint64_t>(pkt.packet.data.Length());
        else
            stats.bytesSentWifi += static_cast<uint64_t>(pkt.packet.data.Length());

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