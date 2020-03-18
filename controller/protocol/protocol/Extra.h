#pragma once
#include "Interface.h"
#include "../../../tools/Buffers.h"
#include "../../PrivateDefines.h"
#include "../../net/NetworkSocket.h"

namespace tgvoip
{
struct Extra : public Serializable, MultiChoice<Extra>
{
    static std::shared_ptr<T> chooseFromType(uint8_t type);
};

struct Codec : public Serializable, SingleChoice<Codec>
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    static const uint32_t Opus = FOURCC('O', 'P', 'U', 'S');

    static const uint32_t Avc = FOURCC('A', 'V', 'C', ' ');
    static const uint32_t Hevc = FOURCC('H', 'E', 'V', 'C');
    static const uint32_t Vp8 = FOURCC('V', 'P', '8', '0');
    static const uint32_t Vp9 = FOURCC('V', 'P', '9', '0');
    static const uint32_t Av1 = FOURCC('A', 'V', '0', '1');

    uint32_t codec = 0;
};

struct StreamInfo : public Serializable, SingleChoice<StreamInfo>
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    enum StreamType : uint8_t
    {
        STREAM_TYPE_AUDIO = 1,
        STREAM_TYPE_VIDEO
    };

    uint8_t streamId = 0;
    StreamType type = STREAM_TYPE_AUDIO;
    Codec codec;
    uint16_t frameDuration = 0;
    bool enabled = false;
};

struct ExtraStreamFlags : public Extra
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    enum Flags : uint8_t
    {
        Enabled = 1,
        Dtx = 2,
        ExtraEC = 3,
        Paused = 4
    };

    uint8_t streamId;
    uint8_t flags = 0;

    static const uint8_t ID = 1;
};

struct ExtraStreamCsd : public Extra
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    uint8_t streamId;
    uint16_t width = 0;
    uint16_t height = 0;

    Array<Bytes> data;

    static const uint8_t ID = 2;
};

struct ExtraLanEndpoint : public Extra
{
public:
    virtual bool parse(const BufferInputStream &in, const VersionInfo &ver);
    virtual void serialize(BufferOutputStream &out, const VersionInfo &ver);

    NetworkAddress address;
    uint16_t port = 0;

    static const uint8_t ID = 3;
};

struct ExtraIpv6Endpoint : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    NetworkAddress address;
    uint16_t port = 0;

    static const uint8_t ID = 7;
};

struct ExtraNetworkChanged : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    enum Flags : uint8_t
    {
        DataSavingEnabled = 1
    };

    uint8_t streamId = 0;
    uint8_t flags = 0;

    static const uint8_t ID = 4;
};

struct ExtraGroupCallKey : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    std::array<uint8_t, 256> key;

    static const uint8_t ID = 5;
};

struct ExtraGroupCallUpgrade : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override { return true; };
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override{};

    static const uint8_t ID = 6;
};

struct ExtraInit : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

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

    Array<Codec> audioCodecs;

    Array<UInt32> decoders;
    uint8_t maxResolution;

    static const uint8_t ID = 8;
};

struct ExtraInitAck : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    uint32_t peerVersion = 0;
    uint32_t minVersion = 0;

    Array<StreamInfo> streams;

    static const uint8_t ID = 8;
};

struct ExtraPing : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override { return true; };
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override{};

    static const uint8_t ID = 9;
};
struct ExtraPong : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    uint32_t seq = 0;

    static const uint8_t ID = 9;
};
} // namespace tgvoip