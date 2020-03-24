#pragma once

namespace tgvoip
{
struct StreamInfo
{
    StreamInfo() = default;
    StreamInfo(uint8_t _id, StreamType _type) : id(_id), type(_type){};

    uint8_t id;
    StreamType type;

    bool enabled = true;
    bool paused = false;
};

struct MediaStreamInfo : public StreamInfo
{
    MediaStreamInfo() = delete;
    int32_t userID;

    uint32_t codec;
};
struct AudioStreamInfo : public MediaStreamInfo
{
    AudioStreamInfo() = delete;
    bool extraECEnabled;
    uint16_t frameDuration;
};
struct VideoStreamInfo : public MediaStreamInfo
{
    VideoStreamInfo() = delete;
    unsigned int width = 0;
    unsigned int height = 0;
    uint16_t rotation = 0;

    std::vector<Buffer> codecSpecificData;
    bool csdIsValid = false;

    int resolution;
};

struct IncomingStream : public StreamInfo
{
    IncomingStream() = delete;
    IncomingStream(uint8_t id, StreamType type) : StreamInfo(id, type){};
};
} // namespace tgvoip

#include "packets/PacketSender.h"