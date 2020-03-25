#include "../../VoIPController.h"

using namespace tgvoip;


#pragma mark - Video

void VoIPController::SetVideoSource(video::VideoSource *source)
{
    std::shared_ptr<OutgoingVideoStream> stm = GetStreamByTypeShared<OutgoingVideoStream>();
    if (!stm)
    {
        LOGE("Can't set video source when there is no outgoing video stream");
        return;
    }

    if (source)
    {
        if (!stm->enabled)
        {
            stm->enabled = true;
            messageThread.Post([this, stm] { SendStreamFlags(*stm); });
        }

        if (!stm->packetSender)
            stm->packetSender = std::make_unique<video::VideoPacketSender>(this, stm, source);
        else
            dynamic_cast<video::VideoPacketSender *>(stm->packetSender.get())->SetSource(source);
    }
    else
    {
        if (stm->enabled)
        {
            stm->enabled = false;
            messageThread.Post([this, stm] { SendStreamFlags(*stm); });
        }
        if (stm->packetSender)
        {
            dynamic_cast<video::VideoPacketSender *>(stm->packetSender.get())->SetSource(NULL);
        }
    }
}

void VoIPController::SetVideoRenderer(video::VideoRenderer *renderer)
{
    videoRenderer = renderer;
}

void VoIPController::SetVideoCodecSpecificData(const std::vector<Buffer> &data)
{
    auto *stm = GetStreamByType<OutgoingVideoStream>();
    stm->codecSpecificData.clear();
    for (const Buffer &csd : data)
    {
        stm->codecSpecificData.push_back(Buffer::CopyOf(csd));
    }
    LOGI("Set outgoing video stream CSD");
}

void VoIPController::ProcessIncomingVideoFrame(Buffer frame, uint32_t pts, bool keyframe, uint16_t rotation)
{
    //LOGI("Incoming video frame size %u pts %u", (unsigned int)frame.Length(), pts);
    if (frame.Length() == 0)
    {
        LOGE("EMPTY FRAME");
    }
    if (videoRenderer)
    {
        auto *stm = GetStreamByType<IncomingVideoStream>();
        size_t offset = 0;
        if (keyframe)
        {
            BufferInputStream in(frame);
            uint16_t width = in.ReadUInt16();
            uint16_t height = in.ReadUInt16();
            uint8_t sizeAndFlag = in.ReadByte();
            int size = sizeAndFlag & 0x0F;
            bool reset = (sizeAndFlag & 0x80) == 0x80;
            if (reset || !stm->csdIsValid || stm->width != width || stm->height != height)
            {
                stm->width = width;
                stm->height = height;
                stm->codecSpecificData.clear();
                for (int i = 0; i < size; i++)
                {
                    size_t len = in.ReadByte();
                    Buffer b(len);
                    in.ReadBytes(b);
                    stm->codecSpecificData.push_back(move(b));
                }
                stm->csdIsValid = false;
            }
            else
            {
                for (int i = 0; i < size; i++)
                {
                    size_t len = in.ReadByte();
                    in.Seek(in.GetOffset() + len);
                }
            }
            offset = in.GetOffset();
        }
        if (!stm->csdIsValid && stm->width && stm->height)
        {
            videoRenderer->Reset(stm->codec, stm->width, stm->height, stm->codecSpecificData);
            stm->csdIsValid = true;
        }
        if (lastReceivedVideoFrameNumber == UINT32_MAX || lastReceivedVideoFrameNumber == pts - 1 || keyframe)
        {
            lastReceivedVideoFrameNumber = pts;
            //LOGV("3 before decode %u", (unsigned int)frame.Length());
            if (stm->rotation != rotation)
            {
                stm->rotation = rotation;
                videoRenderer->SetRotation(rotation);
            }
            if (offset == 0)
            {
                videoRenderer->DecodeAndDisplay(move(frame), pts);
            }
            else
            {
                videoRenderer->DecodeAndDisplay(Buffer::CopyOf(frame, offset, frame.Length() - offset), pts);
            }
        }
        else
        {
            LOGW("Skipping non-keyframe after packet loss...");
        }
    }
}

void VoIPController::SetupOutgoingVideoStream()
{
    vector<uint32_t> myEncoders = video::VideoSource::GetAvailableEncoders();
    auto vstm = std::make_shared<OutgoingVideoStream>();

    if (find(myEncoders.begin(), myEncoders.end(), Codec::Hevc) != myEncoders.end() && find(peerVideoDecoders.begin(), peerVideoDecoders.end(), Codec::Hevc) != peerVideoDecoders.end())
    {
        vstm->codec = Codec::Hevc;
    }
    else if (find(myEncoders.begin(), myEncoders.end(), Codec::Avc) != myEncoders.end() && find(peerVideoDecoders.begin(), peerVideoDecoders.end(), Codec::Avc) != peerVideoDecoders.end())
    {
        vstm->codec = Codec::Avc;
    }
    else if (find(myEncoders.begin(), myEncoders.end(), Codec::Vp8) != myEncoders.end() && find(peerVideoDecoders.begin(), peerVideoDecoders.end(), Codec::Vp8) != peerVideoDecoders.end())
    {
        vstm->codec = Codec::Vp8;
    }
    else
    {
        LOGW("Can't setup outgoing video stream: no codecs in common");
        return;
    }

    vstm->enabled = false;
    outgoingStreams.push_back(dynamic_pointer_cast<OutgoingStream>(vstm));
}
