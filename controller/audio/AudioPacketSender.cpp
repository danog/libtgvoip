#include "AudioPacketSender.h"
#include "../../VoIPController.h"

using namespace tgvoip;

AudioPacketSender::AudioPacketSender(VoIPController *controller, const std::shared_ptr<OutgoingAudioStream> &_stream, const std::shared_ptr<OpusEncoder> &encoder) : PacketSender(controller, dynamic_pointer_cast<OutgoingStream<>>(_stream)), stream(_stream)
{
    SetSource(encoder);
}

void AudioPacketSender::SetSource(const std::shared_ptr<OpusEncoder> &encoder)
{
    if (this->encoder == encoder || !encoder)
        return;
    this->encoder = encoder;

    encoder->SetCallback(std::bind(&AudioPacketSender::SendFrame, this, placeholders::_1, placeholders::_2, placeholders::_3, placeholders::_4));
}

void AudioPacketSender::SendFrame(unsigned char *data, size_t len, unsigned char *secondaryData, size_t secondaryLen)
{
    if (controller->stopping)
        return;

    std::shared_ptr<Packet> pkt = std::make_shared<Packet>();
    pkt->prepare(packetManager); // Populate seqno (aka PTS), ack mask if viable
    pkt->data = std::make_unique<Buffer>(len);
    pkt->data->CopyFrom(data, 0, len);

    if (secondaryLen)
    {
        std::shared_ptr<Buffer> secondaryPtr = std::make_shared<Buffer>(secondaryLen);
        secondaryPtr->CopyFrom(secondaryData, 0, secondaryLen);
        ecAudioPackets.push_back(std::move(secondaryPtr));
        if (ecAudioPackets.size() == 9)
        {
            ecAudioPackets.pop_front();
        }
        /*
        uint8_t fecCount = std::min(static_cast<uint8_t>(ecAudioPackets.size()), extraEcLevel);
        pkt.WriteByte(fecCount);
        for (auto ecData = ecAudioPackets.end() - fecCount; ecData != ecAudioPackets.end(); ++ecData)
        {
            pkt.WriteByte(static_cast<uint8_t>(ecData->Length()));
            pkt.WriteBytes(*ecData);
        }*/
    }
    //LOGE("SEND: For pts %u = seq %u, using seq %u", audioTimestampOut, audioTimestampOut/60 + 1, packetManager.getLocalSeq());

    controller->messageThread.Post([this, pkt]() {
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
        {
            LOGV("waiting for queue, dropping outgoing audio packet, %d %d %d [%d]", (unsigned int)unsentStreamPackets, waitingForAcks, dontSendPackets, maxUnsentStreamPackets);
            return;
        }*/
        //LOGV("Audio packet size %u", (unsigned int)len);
        if (!controller->receivedInitAck)
            return;

        if (!packetLoss)
        {
            controller->SendPacket(std::move(*pkt));
        }
        else
        {
            double retry = stream->frameDuration / (resendCount * 4.0);

            controller->SendPacket(std::move(*pkt), retry / 1000.0, (stream->frameDuration * 4) / 1000.0, resendCount);
        }
    });

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
    if (audioPreprocDataCallback)
    {
        int size = opus_decode(preprocDecoder.get(), data, len, preprocBuffer, 4096, 0);
        audioPreprocDataCallback(preprocBuffer, size);
    }
#endif
}

double AudioPacketSender::setPacketLoss(double percent)
{
    packetLoss = percent;

    if (percent > 2)
    {
        resendCount = std::clamp(percent / 2, 0.0, 4.0);
    }
    /*else if (percent > 5)
    {
        resendCount = 1.5;
    }
    else if (percent > 2)
    {
        resendCount = 1.3;
    }*/
    else
    {
        resendCount = 1;
    }

    ++resendCount;
    double newLoss = percent / resendCount;
    LOGE("Packet loss %lf / resend count %lf = new packet loss %lf", percent, resendCount, newLoss);

    return newLoss;
}
