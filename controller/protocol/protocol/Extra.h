#pragma once
#include "../../../tools/Buffers.h"
#include "../../net/NetworkSocket.h"
#include "Interface.h"

#define FOURCC(a, b, c, d) ((uint32_t)d | ((uint32_t)c << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24))
#define PRINT_FOURCC(x) (char)(x >> 24), (char)(x >> 16), (char)(x >> 8), (char)x

namespace tgvoip
{
struct VersionInfo;
struct Extra : public Serializable, MultiChoice<Extra>
{
    static std::shared_ptr<Extra> choose(const BufferInputStream &in, const VersionInfo &ver);
    static std::shared_ptr<Extra> chooseFromType(uint8_t type);
    uint8_t chooseType(int peerVersion) const;
};

struct Codec : public Serializable, SingleChoice<Codec>
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    enum : uint32_t
    {
        Opus = FOURCC('O', 'P', 'U', 'S'),

        Avc = FOURCC('A', 'V', 'C', ' '),
        Hevc = FOURCC('H', 'E', 'V', 'C'),
        Vp8 = FOURCC('V', 'P', '8', '0'),
        Vp9 = FOURCC('V', 'P', '9', '0'),
        Av1 = FOURCC('A', 'V', '0', '1')
    };
    uint32_t codec = 0;
};

struct StreamInfo : public Serializable, SingleChoice<StreamInfo>
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    enum Type : uint8_t
    {
        Signaling = 0,
        Audio = 1,
        Video = 2
    };

    uint8_t streamId = 0;
    Type type = Type::Audio;
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

    uint8_t getID() const
    {
        return ID;
    }
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

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 2;
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

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 4;
};

struct ExtraLanEndpoint : public Extra
{
public:
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    NetworkAddress address;
    uint16_t port = 0;

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 3;
};

struct ExtraIpv6Endpoint : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    NetworkAddress address;
    uint16_t port = 0;

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 7;
};

struct ExtraGroupCallKey : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    std::array<uint8_t, 256> key;

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 5;
};

struct ExtraGroupCallUpgrade : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override { return true; };
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override{};

    uint8_t getID() const
    {
        return ID;
    }
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

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 8;
};

struct ExtraInitAck : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    uint32_t peerVersion = 0;
    uint32_t minVersion = 0;

    Array<StreamInfo> streams;

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 9;
};

struct ExtraPing : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override { return true; };
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override{};

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 10;
};
struct ExtraPong : public Extra
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override;
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override;

    uint32_t seq = 0;

    uint8_t getID() const
    {
        return ID;
    }
    static const uint8_t ID = 11;
};
} // namespace tgvoip