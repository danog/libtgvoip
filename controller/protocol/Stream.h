#pragma once
#include <vector>
#include <cstdint>
#include "../../tools/Buffers.h"
#include "protocol/Index.h"

namespace tgvoip
{
struct StreamInfo
{
    StreamInfo() = delete;
    StreamInfo(uint8_t _id, StreamType _type) : id(_id), type(_type){};
    virtual ~StreamInfo() = default;

    uint8_t id;
    StreamType type;

    bool enabled = true;
    bool paused = false;
};

struct MediaStreamInfo
{
    virtual ~MediaStreamInfo() = default;

    int32_t userID;

    uint32_t codec;
};
struct AudioStreamInfo
{
    virtual ~AudioStreamInfo() = default;
    
    bool extraECEnabled;
    uint16_t frameDuration;
};
struct VideoStreamInfo
{
    virtual ~VideoStreamInfo() = default;

    unsigned int width = 0;
    unsigned int height = 0;
    uint16_t rotation = 0;

    std::vector<Buffer> codecSpecificData;
    bool csdIsValid = false;

    int resolution;
};

} // namespace tgvoip

#include "packets/PacketSender.h"