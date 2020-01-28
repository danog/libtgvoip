#include "AudioPacketSender.h"
#include "../PrivateDefines.h"

using namespace tgvoip;

AudioPacketSender::AudioPacketSender(VoIPController *controller, const std::shared_ptr<OpusEncoder> &encoder, const std::shared_ptr<VoIPController::Stream> &stream) : PacketSender(controller, stream), encoder(encoder)
{
    SetSource(encoder);
}

AudioPacketSender::~AudioPacketSender()
{
}

void AudioPacketSender::SetSource(const std::shared_ptr<OpusEncoder> &encoder)
{
    if (this->encoder == encoder || !encoder)
        return;
    this->encoder = encoder;

    encoder->SetCallback(bind(&AudioPacketSender::SendFrame, this, placeholders::_1, placeholders::_2, placeholders::_3, placeholders::_4));
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
        /*{
            LOGV("waiting for queue, dropping outgoing audio packet, %d %d %d [%d]", (unsigned int)unsentStreamPackets, waitingForAcks, dontSendPackets, maxUnsentStreamPackets);
            return;
        }*/
        //LOGV("Audio packet size %u", (unsigned int)len);
        if (!ReceivedInitAck())
            return;

        BufferOutputStream pkt(1500);

        bool hasExtraFEC = PeerVersion() >= 7 && secondaryLen && shittyInternetMode;
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

        //unsentStreamPackets++;

        //PendingOutgoingPacket p{
        //    /*.seq=*/nextLocalSeq(),
        //    /*.type=*/PKT_STREAM_DATA,
        //    /*.len=*/pkt.GetLength(),
        //    /*.data=*/Buffer(move(pkt)),
        //    /*.endpoint=*/0,
        //};

        //conctl.PacketSent(p.seq, p.len);

        //shared_ptr<VoIPController::Stream> outgoingAudioStream = GetStreamByType(STREAM_TYPE_AUDIO, false);

        if (PeerVersion() < PROTOCOL_RELIABLE)
        {
            double rtt = LastRtt();

            rtt = !rtt || rtt > 0.3 ? 0.5 : rtt; // Tweak this (a lot) later

            double timeout = 0; //(outgoingAudioStream && outgoingAudioStream->jitterBuffer ? outgoingAudioStream->jitterBuffer->GetTimeoutWindow() : 0) - rtt;
            LOGE("TIMEOUT %lf", timeout + rtt);

            timeout = timeout <= 0 ? rtt : timeout;

            SendPacketReliably(PKT_STREAM_DATA, pkt.GetBuffer(), pkt.GetLength(), rtt, timeout, 10); // Todo Optimize RTT
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
            ecBuf.CopyFrom(*secondaryDataBufPtr, 0, secondaryLen);
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
