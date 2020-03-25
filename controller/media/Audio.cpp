#include "../../VoIPController.h"

using namespace tgvoip;

#pragma mark - Audio I/O

void VoIPController::InitializeAudio()
{
    double t = GetCurrentTime();
    auto *outgoingAudioStream = GetStreamByType<OutgoingAudioStream>();
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
    echoCanceller = std::make_unique<EchoCanceller>(config.enableAEC, config.enableNS, config.enableAGC);
    encoder = std::make_shared<OpusEncoder>(audioInput, true);
    encoder->SetOutputFrameDuration(outgoingAudioStream->frameDuration);
    encoder->SetEchoCanceller(echoCanceller);
    encoder->SetSecondaryEncoderEnabled(false);
    if (config.enableVolumeControl)
    {
        encoder->AddAudioEffect(inputVolume);
    }

    dynamic_cast<AudioPacketSender *>(outgoingAudioStream->packetSender.get())->SetSource(encoder);

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
    auto *stm = GetStreamByID<IncomingAudioStream>(StreamId::Audio);
    stm->decoder = std::make_shared<OpusDecoder>(audioOutput, true, ver.peerVersion >= 6);
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
    for (auto &s : incomingStreams)
    {
        if (s->type == StreamType::Audio && s->enabled)
        {
            areAnyAudioStreamsEnabled = true;
            break;
        }
    }
    if (audioOutput)
    {
        LOGV("New audio output state: %d (prev %d)", areAnyAudioStreamsEnabled, audioOutput->IsPlaying());
        if (audioOutput->IsPlaying() != areAnyAudioStreamsEnabled)
        {
            if (areAnyAudioStreamsEnabled)
                audioOutput->Start();
            else
                audioOutput->Stop();
        }
    }
}
