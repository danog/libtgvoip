#pragma once

namespace tgvoip
{
struct StreamInfo
{
    StreamInfo() = delete;
    StreamInfo(uint8_t _id, StreamType _type) : id(_id), type(_type){};

    uint8_t id;
    StreamType type;

    bool enabled = true;
    bool paused = false;
};

struct MediaStreamInfo
{
    int32_t userID;

    uint32_t codec;
};
struct AudioStreamInfo : public MediaStreamInfo
{
    bool extraECEnabled;
    uint16_t frameDuration;
};
struct VideoStreamInfo : public MediaStreamInfo
{
    unsigned int width = 0;
    unsigned int height = 0;
    uint16_t rotation = 0;
    int resolution;
};

using IncomingStream = StreamInfo;
} // namespace tgvoip

#include "packets/PacketSender.h"