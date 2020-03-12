#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

#pragma mark - Video

void VoIPController::SetVideoSource(video::VideoSource *source)
{
    shared_ptr<Stream> stm = GetStreamByType(STREAM_TYPE_VIDEO, true);
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
            stm->packetSender = std::make_unique<video::VideoPacketSender>(this, source, stm);
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
    outgoingStreams[1]->codecSpecificData.clear();
    for (const Buffer &csd : data)
    {
        outgoingStreams[1]->codecSpecificData.push_back(Buffer::CopyOf(csd));
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
        shared_ptr<Stream> stm = GetStreamByType(STREAM_TYPE_VIDEO, false);
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
    shared_ptr<Stream> vstm = make_shared<Stream>();
    vstm->id = 2;
    vstm->type = STREAM_TYPE_VIDEO;

    if (find(myEncoders.begin(), myEncoders.end(), CODEC_HEVC) != myEncoders.end() && find(peerVideoDecoders.begin(), peerVideoDecoders.end(), CODEC_HEVC) != peerVideoDecoders.end())
    {
        vstm->codec = CODEC_HEVC;
    }
    else if (find(myEncoders.begin(), myEncoders.end(), CODEC_AVC) != myEncoders.end() && find(peerVideoDecoders.begin(), peerVideoDecoders.end(), CODEC_AVC) != peerVideoDecoders.end())
    {
        vstm->codec = CODEC_AVC;
    }
    else if (find(myEncoders.begin(), myEncoders.end(), CODEC_VP8) != myEncoders.end() && find(peerVideoDecoders.begin(), peerVideoDecoders.end(), CODEC_VP8) != peerVideoDecoders.end())
    {
        vstm->codec = CODEC_VP8;
    }
    else
    {
        LOGW("Can't setup outgoing video stream: no codecs in common");
        return;
    }

    vstm->enabled = false;
    outgoingStreams.push_back(vstm);
}