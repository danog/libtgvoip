#include "AudioPacketSender.h"
#include "../PrivateDefines.h"

using namespace tgvoip;

AudioPacketSender::AudioPacketSender(VoIPController *controller, const std::shared_ptr<OutgoingAudioStream> &stream, const std::shared_ptr<OpusEncoder> &encoder) : PacketSender(controller, stream)
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
    if (IsStopping())
        return;

    Buffer dataBuf = outgoingAudioBufferPool.Get();
    Buffer secondaryDataBuf = secondaryLen && secondaryData ? outgoingAudioBufferPool.Get() : Buffer();
    dataBuf.CopyFrom(data, 0, len);
    if (secondaryLen && secondaryData)
    {
        secondaryDataBuf.CopyFrom(secondaryData, 0, secondaryLen);
    }
    shared_ptr<Buffer> dataBufPtr = make_shared<Buffer>(move(dataBuf));
    shared_ptr<Buffer> secondaryDataBufPtr = make_shared<Buffer>(move(secondaryDataBuf));

    GetMessageThread().Post([this, dataBufPtr, secondaryDataBufPtr, len, secondaryLen]() {
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
        if (!ReceivedInitAck())
            return;

        BufferOutputStream pkt(1500);

        bool hasExtraFEC = PeerVersion() >= 7 && secondaryLen && shittyInternetMode;
        uint8_t flags = static_cast<uint8_t>(len > 255 || hasExtraFEC ? STREAM_DATA_FLAG_LEN16 : 0);
        pkt.WriteByte(flags | 1); // flags + streamID

        if (flags & STREAM_DATA_FLAG_LEN16)
        {
            int16_t lenAndFlags = static_cast<int16_t>(len);
            if (hasExtraFEC)
                lenAndFlags |= STREAM_DATA_XFLAG_EXTRA_FEC;
            pkt.WriteInt16(lenAndFlags);
        }
        else
        {
            pkt.WriteByte(static_cast<uint8_t>(len));
        }

        pkt.WriteInt32(audioTimestampOut);
        pkt.WriteBytes(*dataBufPtr, 0, len);

        //LOGE("SEND: For pts %u = seq %u, using seq %u", audioTimestampOut, audioTimestampOut/60 + 1, packetManager.getLocalSeq());

        if (hasExtraFEC)
        {
            Buffer ecBuf(secondaryLen);
            ecBuf.CopyFromOtherBuffer(*secondaryDataBufPtr, secondaryLen);
            ecAudioPackets.push_back(move(ecBuf));
            if (ecAudioPackets.size() > 4)
            {
                ecAudioPackets.pop_front();
            }

            uint8_t fecCount = std::min(static_cast<uint8_t>(ecAudioPackets.size()), extraEcLevel);
            pkt.WriteByte(fecCount);
            for (auto ecData = ecAudioPackets.end() - fecCount; ecData != ecAudioPackets.end(); ++ecData)
            {
                pkt.WriteByte(static_cast<uint8_t>(ecData->Length()));
                pkt.WriteBytes(*ecData);
            }
        }

        //unsentStreamPackets++;

        if (PeerVersion() < PROTOCOL_RELIABLE)
        {
            // Need to increase this anyway to go hand in hand with timestamp
            // packetManager.nextLocalSeq();

            if (!packetLoss)
            {
                PendingOutgoingPacket p{
                    0,
                    PKT_STREAM_DATA,
                    pkt.GetLength(),
                    Buffer(move(pkt)),
                    0,
                };

                SendPacket(std::move(p));
            }
            else
            {
                double retry = stream->frameDuration / (resendCount * 4.0);

                SendPacketReliably(PKT_STREAM_DATA, pkt.GetBuffer(), pkt.GetLength(), retry / 1000.0, (stream->frameDuration * 4) / 1000.0, resendCount); // Todo Optimize RTT
            }
        }
        else
        {
            PendingOutgoingPacket p{
                /*.seq=*/0,
                /*.type=*/PKT_STREAM_DATA,
                /*.len=*/pkt.GetLength(),
                /*.data=*/Buffer(move(pkt)),
                /*.endpoint=*/0,
            };

            SendPacket(move(p));
        }
        if (PeerVersion() < 7 && secondaryLen && shittyInternetMode)
        {
            Buffer ecBuf(secondaryLen);
            ecBuf.CopyFromOtherBuffer(*secondaryDataBufPtr, secondaryLen);
            if (ecAudioPackets.size() == 4)
            {
                ecAudioPackets.pop_front();
            }
            ecAudioPackets.push_back(move(ecBuf));
            pkt = BufferOutputStream(1500);
            pkt.WriteByte(stream->id);
            pkt.WriteInt32(audioTimestampOut);
            uint8_t fecCount = std::min(static_cast<uint8_t>(ecAudioPackets.size()), extraEcLevel);
            pkt.WriteByte(fecCount);
            for (auto ecData = ecAudioPackets.end() - fecCount; ecData != ecAudioPackets.end(); ++ecData)
            {
                pkt.WriteByte((unsigned char)ecData->Length());
                pkt.WriteBytes(*ecData);
            }

            PendingOutgoingPacket p{
                0,
                PKT_STREAM_EC,
                pkt.GetLength(),
                Buffer(move(pkt)),
                0};
            SendPacket(std::move(p));
        }

        audioTimestampOut += stream->frameDuration;
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