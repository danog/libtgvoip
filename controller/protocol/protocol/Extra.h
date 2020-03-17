#pragma once
#include "Interface.h"
#include "../../../tools/Buffers.h"
#include "../../PrivateDefines.h"

namespace tgvoip
{
struct Extra : public Serializable
{
    static std::shared_ptr<Extra> choose(const BufferInputStream &in, VersionInfo &ver);
};

struct StreamInfo : public Serializable, SingleChoice<StreamInfo>
{
public:
    bool parse(const BufferInputStream &in, VersionInfo &ver) override;
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override;

    enum StreamType : uint8_t
    {
        STREAM_TYPE_AUDIO = 1,
        STREAM_TYPE_VIDEO
    };

    uint8_t streamId = 0;
    StreamType type = STREAM_TYPE_AUDIO;
    uint32_t codec = 0;
    uint16_t frameDuration = 0;
    bool enabled = false;
};

struct ExtraStreamFlags : public Extra
{
public:
    bool parse(const BufferInputStream &in, VersionInfo &ver) override;
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override;

    enum Flags : uint8_t
    {
        Enabled = 1,
        Dtx = 2,
        ExtraEC = 3,
        Paused = 4
    };

    uint8_t flags = 0;

    static const uint8_t ID = 1;
};

struct ExtraStreamCsd : public Extra
{
public:
    bool parse(const BufferInputStream &in, VersionInfo &ver) override;
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override;

    uint16_t width = 0;
    uint16_t height = 0;

    std::vector<BufferInputStream> data;

    static const uint8_t ID = 2;
};

struct ExtraLanEndpoint : public Extra
{
public:
    virtual bool parse(const BufferInputStream &in, VersionInfo &ver);
    virtual bool serialize(BufferOutputStream &out, VersionInfo &ver);

    NetworkAddress address;
    uint16_t port = 0;

    static const uint8_t ID = 3;
};

struct ExtraIpv6Endpoint : public Extra
{
    bool parse(const BufferInputStream &in, VersionInfo &ver) override;
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override;

    NetworkAddress address;
    uint16_t port = 0;

    static const uint8_t ID = 7;
};

struct ExtraNetworkChanged : public Extra
{
    bool parse(const BufferInputStream &in, VersionInfo &ver) override;
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override;

    enum Flags : uint8_t
    {
        DataSavingEnabled = 1
    };

    uint8_t flags = 0;

    static const uint8_t ID = 4;
};

struct ExtraGroupCallKey : public Extra
{
    bool parse(const BufferInputStream &in, VersionInfo &ver) override;
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override;

    std::array<uint8_t, 256> key;

    static const uint8_t ID = 5;
};

struct ExtraGroupCallUpgrade : public Extra
{
    bool parse(const BufferInputStream &in, VersionInfo &ver) override { return true; };
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override { return true; };

    static const uint8_t ID = 6;
};

struct ExtraInit : public Extra
{
    bool parse(const BufferInputStream &in, VersionInfo &ver) override;
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override;

    enum Flags : uint8_t
    {
        DataSavingEnabled = 1,
        GroupCallSupported = 2,
        VideoSendSupported = 4,
        VideoRecvSupported = 8
    };

    uint32_t peerVersion = 0;
    uint32_t minVersion = 0;
    uint8_t flags = 0;


    static const uint8_t ID = 8;
};

struct ExtraInitAck : public Extra
{
    bool parse(const BufferInputStream &in, VersionInfo &ver) override;
    bool serialize(BufferOutputStream &out, VersionInfo &ver) override;

    uint32_t peerVersion = 0;
    uint32_t minVersion = 0;

    Array<StreamInfo> streams;

    static const uint8_t ID = 8;
};

} // namespace tgvoip