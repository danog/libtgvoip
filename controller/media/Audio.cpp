
#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

#pragma mark - Audio I/O

void VoIPController::HandleAudioInput(unsigned char *data, size_t len, unsigned char *secondaryData, size_t secondaryLen)
{
    if (stopping)
        return;

    // TODO make an AudioPacketSender

    Buffer dataBuf = outgoingAudioBufferPool.Get();
    Buffer secondaryDataBuf = secondaryLen && secondaryData ? outgoingAudioBufferPool.Get() : Buffer();
    dataBuf.CopyFrom(data, 0, len);
    if (secondaryLen && secondaryData)
    {
        secondaryDataBuf.CopyFrom(secondaryData, 0, secondaryLen);
    }
    shared_ptr<Buffer> dataBufPtr = make_shared<Buffer>(move(dataBuf));
    shared_ptr<Buffer> secondaryDataBufPtr = make_shared<Buffer>(move(secondaryDataBuf));

    messageThread.Post([this, dataBufPtr, secondaryDataBufPtr, len, secondaryLen]() {
        /*
        unsentStreamPacketsHistory.Add(static_cast<unsigned int>(unsentStreamPackets));
        if (unsentStreamPacketsHistory.Average() >= maxUnsentStreamPackets && !videoPacketSender)
        {
            LOGW("Resetting stalled send queue");
            sendQueue.clear();
            unsentStreamPacketsHistory.Reset();
            unsentStreamPackets = 0;
        }
        //if (waitingForAcks || dontSendPackets > 0 || ((unsigned int)unsentStreamPackets >= maxUnsentStreamPackets))
        /*{
            LOGV("waiting for queue, dropping outgoing audio packet, %d %d %d [%d]", (unsigned int)unsentStreamPackets, waitingForAcks, dontSendPackets, maxUnsentStreamPackets);
            return;
        }*/
        //LOGV("Audio packet size %u", (unsigned int)len);
        if (!receivedInitAck)
            return;

        BufferOutputStream pkt(1500);

        bool hasExtraFEC = peerVersion >= 7 && secondaryLen && shittyInternetMode;
        unsigned char flags = (unsigned char)(len > 255 || hasExtraFEC ? STREAM_DATA_FLAG_LEN16 : 0);
        pkt.WriteByte((unsigned char)(1 | flags)); // streamID + flags
        if (len > 255 || hasExtraFEC)
        {
            int16_t lenAndFlags = static_cast<int16_t>(len);
            if (hasExtraFEC)
                lenAndFlags |= STREAM_DATA_XFLAG_EXTRA_FEC;
            pkt.WriteInt16(lenAndFlags);
        }
        else
        {
            pkt.WriteByte((unsigned char)len);
        }
        pkt.WriteInt32(audioTimestampOut);
        pkt.WriteBytes(*dataBufPtr, 0, len);

        if (hasExtraFEC)
        {
            Buffer ecBuf(secondaryLen);
            ecBuf.CopyFrom(*secondaryDataBufPtr, 0, secondaryLen);
            if (ecAudioPackets.size() == 4)
            {
                ecAudioPackets.pop_front();
            }
            ecAudioPackets.push_back(move(ecBuf));
            uint8_t fecCount = std::min(static_cast<uint8_t>(ecAudioPackets.size()), extraEcLevel);
            pkt.WriteByte(fecCount);
            for (auto ecData = ecAudioPackets.end() - fecCount; ecData != ecAudioPackets.end(); ++ecData)
            {
                pkt.WriteByte((unsigned char)ecData->Length());
                pkt.WriteBytes(*ecData);
            }
        }

        unsentStreamPackets++;

        //PendingOutgoingPacket p{
        //    /*.seq=*/GenerateOutSeq(),
        //    /*.type=*/PKT_STREAM_DATA,
        //    /*.len=*/pkt.GetLength(),
        //    /*.data=*/Buffer(move(pkt)),
        //    /*.endpoint=*/0,
        //};

        //conctl.PacketSent(p.seq, p.len);

        shared_ptr<Stream> outgoingAudioStream = GetStreamByType(STREAM_TYPE_AUDIO, false);
        
        double rtt = rttHistory[0];

        rtt = !rtt || rtt > 0.3 ? 0.5 : rtt; // Tweak this (a lot) later

        double timeout = (outgoingAudioStream && outgoingAudioStream->jitterBuffer ? outgoingAudioStream->jitterBuffer->GetTimeoutWindow() : 0) - rtt;
        LOGE("TIMEOUT %lf", timeout + rtt);

        timeout = timeout <= 0 ? rtt : timeout;
        
        SendPacketReliably(PKT_STREAM_DATA, pkt.GetBuffer(), pkt.GetLength(), rtt, timeout, 10); // Todo Optimize RTT
        //SendOrEnqueuePacket(move(p));
        if (peerVersion < 7 && secondaryLen && shittyInternetMode)
        {
            Buffer ecBuf(secondaryLen);
            ecBuf.CopyFrom(*secondaryDataBufPtr, 0, secondaryLen);
            if (ecAudioPackets.size() == 4)
            {
                ecAudioPackets.pop_front();
            }
            ecAudioPackets.push_back(move(ecBuf));
            pkt = BufferOutputStream(1500);
            pkt.WriteByte(outgoingStreams[0]->id);
            pkt.WriteInt32(audioTimestampOut);
            uint8_t fecCount = std::min(static_cast<uint8_t>(ecAudioPackets.size()), extraEcLevel);
            pkt.WriteByte(fecCount);
            for (auto ecData = ecAudioPackets.end() - fecCount; ecData != ecAudioPackets.end(); ++ecData)
            {
                pkt.WriteByte((unsigned char)ecData->Length());
                pkt.WriteBytes(*ecData);
            }

            PendingOutgoingPacket p{
                GenerateOutSeq(),
                PKT_STREAM_EC,
                pkt.GetLength(),
                Buffer(move(pkt)),
                0};
            SendOrEnqueuePacket(move(p));
        }

        audioTimestampOut += outgoingStreams[0]->frameDuration;
    });

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
    if (audioPreprocDataCallback)
    {
        int size = opus_decode(preprocDecoder.get(), data, len, preprocBuffer, 4096, 0);
        audioPreprocDataCallback(preprocBuffer, size);
    }
#endif
}

void VoIPController::InitializeAudio()
{
    double t = GetCurrentTime();
    shared_ptr<Stream> outgoingAudioStream = GetStreamByType(STREAM_TYPE_AUDIO, true);
    LOGI("before create audio io");
    audioIO = audio::AudioIO::Create(currentAudioInput, currentAudioOutput);
    audioInput = audioIO->GetInput();
    audioOutput = audioIO->GetOutput();
#ifdef __ANDROID__
    audio::AudioInputAndroid *androidInput = dynamic_cast<audio::AudioInputAndroid *>(audioInput.get());
    if (androidInput)
    {
        unsigned int effects = androidInput->GetEnabledEffects();
        if (!(effects & audio::AudioInputAndroid::EFFECT_AEC))
        {
            config.enableAEC = true;
            LOGI("Forcing software AEC because built-in is not good");
        }
        if (!(effects & audio::AudioInputAndroid::EFFECT_NS))
        {
            config.enableNS = true;
            LOGI("Forcing software NS because built-in is not good");
        }
    }
#elif defined(__APPLE__) && TARGET_OS_OSX
    SetAudioOutputDuckingEnabled(macAudioDuckingEnabled);
#endif
    LOGI("AEC: %d NS: %d AGC: %d", config.enableAEC, config.enableNS, config.enableAGC);
    echoCanceller.reset(new EchoCanceller(config.enableAEC, config.enableNS, config.enableAGC));
    encoder.reset(new OpusEncoder(audioInput, true));
    encoder->SetCallback(bind(&VoIPController::HandleAudioInput, this, placeholders::_1, placeholders::_2, placeholders::_3, placeholders::_4));
    encoder->SetOutputFrameDuration(outgoingAudioStream->frameDuration);
    encoder->SetEchoCanceller(echoCanceller);
    encoder->SetSecondaryEncoderEnabled(false);
    if (config.enableVolumeControl)
    {
        encoder->AddAudioEffect(inputVolume);
    }

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
    dynamic_cast<audio::AudioInputCallback *>(audioInput.get())->SetDataCallback(audioInputDataCallback);
    dynamic_cast<audio::AudioOutputCallback *>(audioOutput.get())->SetDataCallback(audioOutputDataCallback);
#endif

    if (!audioOutput->IsInitialized())
    {
        LOGE("Error initializing audio playback");
        lastError = ERROR_AUDIO_IO;

        SetState(STATE_FAILED);
        return;
    }
    UpdateAudioBitrateLimit();
    LOGI("Audio initialization took %f seconds", GetCurrentTime() - t);
}

void VoIPController::StartAudio()
{
    OnAudioOutputReady();

    encoder->Start();
    if (!micMuted)
    {
        audioInput->Start();
        if (!audioInput->IsInitialized())
        {
            LOGE("Error initializing audio capture");
            lastError = ERROR_AUDIO_IO;

            SetState(STATE_FAILED);
            return;
        }
    }
}

void VoIPController::OnAudioOutputReady()
{
    LOGI("Audio I/O ready");
    auto &stm = incomingStreams[0];
    stm->decoder = make_shared<OpusDecoder>(audioOutput, true, peerVersion >= 6);
    stm->decoder->SetEchoCanceller(echoCanceller);
    if (config.enableVolumeControl)
    {
        stm->decoder->AddAudioEffect(outputVolume);
    }
    stm->decoder->SetJitterBuffer(stm->jitterBuffer);
    stm->decoder->SetFrameDuration(stm->frameDuration);
    stm->decoder->Start();
}

void VoIPController::UpdateAudioOutputState()
{
    bool areAnyAudioStreamsEnabled = false;
    for (auto s = incomingStreams.begin(); s != incomingStreams.end(); ++s)
    {
        if ((*s)->type == STREAM_TYPE_AUDIO && (*s)->enabled)
            areAnyAudioStreamsEnabled = true;
    }
    if (audioOutput)
    {
        LOGV("New audio output state: %d", areAnyAudioStreamsEnabled);
        if (audioOutput->IsPlaying() != areAnyAudioStreamsEnabled)
        {
            if (areAnyAudioStreamsEnabled)
                audioOutput->Start();
            else
                audioOutput->Stop();
        }
    }
}
