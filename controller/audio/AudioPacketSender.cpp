#include "AudioPacketSender.h"
#include "../../VoIPController.h"

using namespace tgvoip;

AudioPacketSender::AudioPacketSender(VoIPController *controller, std::shared_ptr<OutgoingAudioStream> _stream, const std::shared_ptr<OpusEncoder> &encoder) : PacketSender(controller, dynamic_pointer_cast<OutgoingStream>(_stream)), stream(_stream)
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
    pkt->data = std::make_unique<Buffer>(len);
    pkt->data->CopyFrom(data, 0, len);

    std::shared_ptr<Buffer> secondaryPtr;
    if (secondaryLen)
    {
        secondaryPtr = std::make_shared<Buffer>(secondaryLen);
        secondaryPtr->CopyFrom(secondaryData, 0, secondaryLen);
    }

    controller->messageThread.Post([this, pkt, secondaryPtr]() {
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

        pkt->prepare(packetManager); // Populate seqno (aka PTS), ack mask if viable

        if (secondaryPtr)
        {
            if (shittyInternetMode)
            {
                uint8_t maxEC = std::min(
                    std::min(
                        static_cast<uint8_t>(ecAudioPackets.size()),
                        static_cast<uint8_t>(pkt->seq - 1)),
                    extraEcLevel);

                for (auto ecPkt = std::prev(ecAudioPackets.end(), maxEC); ecPkt != ecAudioPackets.end(); ecPkt++)
                {
                    auto distance = std::distance(ecPkt, ecAudioPackets.end());
                    if (!packetManager.wasLocalAcked(pkt->seq - distance))
                    {
                        pkt->extraEC.v[8 - distance].d = std::make_shared<InputBytes>(*ecPkt);
                    }
                }
            }
            ecAudioPackets.push_back(std::move(secondaryPtr));
            if (ecAudioPackets.size() == 9)
            {
                ecAudioPackets.pop_front();
            }
        }
        //LOGE("SEND: For pts %u = seq %u, using seq %u", audioTimestampOut, audioTimestampOut/60 + 1, packetManager.getLocalSeq());

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
        resendCount = std::clamp(percent / 2, 0.0, 3.0);
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
