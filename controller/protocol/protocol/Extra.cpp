#include "Extra.h"
#include "../../../VoIPController.h"
#include "../Stream.h"

using namespace tgvoip;

bool Codec::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    if (ver.peerVersion >= 5)
        return in.TryRead(codec);

    if (!in.TryReadCompat<uint8_t>(codec))
        return false;

    codec = codec == CODEC_OPUS_OLD ? Codec::Opus : 0;

    return true;
}

void Codec::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    if (ver.peerVersion >= 5)
        return out.WriteUInt32(codec);

    out.WriteByte(codec == Codec::Opus ? 1 : 0);
}

std::shared_ptr<Extra> Extra::choose(const BufferInputStream &in, const VersionInfo &ver)
{
    uint8_t id;
    if (!in.TryRead(id))
    {
        return nullptr;
    }
    unsigned char fullHash[SHA1_LENGTH];
    VoIPController::crypto.sha1(const_cast<uint8_t *>(in.GetRawBuffer() - in.GetOffset()), in.GetLength(), fullHash);

#ifdef LOG_PACKETS
    LOGE("Got extra ID %hhu", id);
#endif

    std::shared_ptr<Extra> res;
    switch (id)
    {
    case ExtraStreamFlags::ID:
        res = std::make_shared<ExtraStreamFlags>();
        break;
    case ExtraStreamCsd::ID:
        res = std::make_shared<ExtraStreamCsd>();
        break;
    case ExtraLanEndpoint::ID:
        res = std::make_shared<ExtraLanEndpoint>();
        break;
    case ExtraIpv6Endpoint::ID:
        res = std::make_shared<ExtraIpv6Endpoint>();
        break;
    case ExtraNetworkChanged::ID:
        res = std::make_shared<ExtraNetworkChanged>();
        break;
    case ExtraGroupCallKey::ID:
        res = std::make_shared<ExtraGroupCallKey>();
        break;
    case ExtraGroupCallUpgrade::ID:
        res = std::make_shared<ExtraGroupCallUpgrade>();
        break;
    case ExtraInit::ID:
        res = std::make_shared<ExtraInit>();
        break;
    case ExtraInitAck::ID:
        res = std::make_shared<ExtraInitAck>();
        break;
    case ExtraPing::ID:
        res = std::make_shared<ExtraPing>();
        break;
    case ExtraPong::ID:
        res = std::make_shared<ExtraPong>();
        break;
    }
    if (res)
        res->hash = *reinterpret_cast<uint64_t *>(fullHash);
    return res;
}
void Extra::choose(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteByte(getID());
}
std::shared_ptr<Extra> Extra::chooseFromType(uint8_t type)
{
    switch (type)
    {
    case PKT_INIT:
        return std::make_shared<ExtraInit>();
    case PKT_INIT_ACK:
        return std::make_shared<ExtraInitAck>();
    case PKT_LAN_ENDPOINT:
        return std::make_shared<ExtraLanEndpoint>();
    case PKT_NETWORK_CHANGED:
        return std::make_shared<ExtraNetworkChanged>();
    case PKT_PING:
        return std::make_shared<ExtraPing>();
    case PKT_PONG:
        return std::make_shared<ExtraPong>();
    case PKT_STREAM_STATE:
        return std::make_shared<ExtraStreamFlags>();
    }
    return nullptr;
}
uint8_t Extra::chooseType(int peerVersion) const
{
    switch (getID())
    {
    case ExtraInit::ID:
        return PKT_INIT;
    case ExtraInitAck::ID:
        return PKT_INIT_ACK;
    case ExtraPing::ID:
        return PKT_PING;
    case ExtraPong::ID:
        return PKT_PONG;
    }
    if (peerVersion < 6)
    {
        switch (getID())
        {
        case ExtraLanEndpoint::ID:
            return PKT_LAN_ENDPOINT;
        case ExtraNetworkChanged::ID:
            return PKT_NETWORK_CHANGED;
        case ExtraStreamFlags::ID:
            return PKT_STREAM_STATE;
        }
    }
    return PKT_NOP;
}
std::string Extra::print() const
{
    switch (getID())
    {
    case ExtraStreamFlags::ID:
        return "ExtraStreamFlags";
    case ExtraStreamCsd::ID:
        return "ExtraStreamCsd";
    case ExtraLanEndpoint::ID:
        return "ExtraLanEndpoint";
    case ExtraIpv6Endpoint::ID:
        return "ExtraIpv6Endpoint";
    case ExtraGroupCallKey::ID:
        return "ExtraGroupCallKey";
    case ExtraGroupCallUpgrade::ID:
        return "ExtraGroupCallUpgrade";
    case ExtraNetworkChanged::ID:
        return "ExtraNetworkChanged";
    case ExtraInit::ID:
        return "ExtraInit";
    case ExtraInitAck::ID:
        return "ExtraInitAck";
    case ExtraPing::ID:
        return "ExtraPing";
    case ExtraPong::ID:
        return "ExtraPong";
    }
    return "???";
}

bool ExtraStreamInfo::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.TryRead(streamId) &&
           in.TryReadCompat<uint8_t>(type) &&
           in.TryRead(codec, ver) &&
           in.TryRead(frameDuration) &&
           in.TryReadCompat<uint8_t>(enabled);
}

void ExtraStreamInfo::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteByte(streamId);
    out.WriteByte(type);
    out.Write(codec, ver);
    out.WriteUInt16(frameDuration);
    out.WriteByte(enabled ? 1 : 0);
}

bool ExtraStreamFlags::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    if (ver.peerVersion < 6)
    {
        if (!in.TryRead(streamId))
            return false;
        if (!in.TryRead(flags))
            return false;
        flags = flags ? Flags::Enabled : 0;
        return true;
    }
    return in.TryRead(streamId) &&
                   ver.isNew()
               ? in.TryRead(flags)
               : in.TryReadCompat<uint32_t>(flags);
}
void ExtraStreamFlags::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteByte(streamId);

    if (ver.isNew())
        out.WriteByte(flags);
    else if (ver.peerVersion >= 6)
        out.WriteUInt32(flags);
    else
        out.WriteByte(flags & Flags::Enabled);
}

bool ExtraStreamCsd::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.TryRead(streamId) &&
           in.TryRead(width) &&
           in.TryRead(height) &&
           in.TryRead(data, ver);
}
void ExtraStreamCsd::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteByte(streamId);
    out.WriteUInt16(width);
    out.WriteUInt16(height);
    data.serialize(out, ver);
}

bool ExtraLanEndpoint::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.TryRead(address, false) &&
           (ver.isNew() ? in.TryRead(port) : in.TryReadCompat<uint32_t>(port));
}
void ExtraLanEndpoint::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteUInt32(address.addr.ipv4);
    ver.isNew() ? out.WriteUInt16(port) : out.WriteUInt32(port);
}

bool ExtraIpv6Endpoint::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.TryRead(address, true) &&
           in.TryRead(port);
}
void ExtraIpv6Endpoint::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteBytes(address.addr.ipv6, sizeof(address.addr.ipv6));
    out.WriteUInt16(port);
}

bool ExtraGroupCallKey::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.TryRead(key);
}
void ExtraGroupCallKey::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteBytes(key);
}

bool ExtraInit::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    /*
    LOGW("Init: ")
    for (auto i = 0; i < in.Remaining(); i++) {
        LOGW("%hhu", in.GetRawBuffer()[i]);
    }*/
    return in.TryRead(peerVersion) &&
           in.TryRead(minVersion) &&
           (ver.isNew() ? in.TryRead(flags) : in.TryReadCompat<uint32_t>(flags)) &&
           in.TryRead(audioCodecs, ver) &&
           in.TryRead(decoders, ver) &&
           in.TryRead(maxResolution);
}
void ExtraInit::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteUInt32(peerVersion);
    out.WriteUInt32(minVersion);
    if (ver.isNew())
        out.WriteByte(flags);
    else
        out.WriteUInt32(flags);
    if (ver.connectionMaxLayer < 74)
    {
        out.WriteByte(2); // audio codecs count, for some reason two codecs required
        out.WriteByte(CODEC_OPUS_OLD);
        out.WriteByte(0);            // second useless codec
        out.WriteByte(0);            // what is this
        out.WriteByte(0);            // what is this
        out.WriteInt32(Codec::Opus); // WHAT IS THIS
        out.WriteByte(0);            // video codecs count (decode)
        out.WriteByte(0);            // video codecs count (encode)
    }
    else
    {
        out.Write(audioCodecs, ver);
        out.Write(decoders, ver);
        out.WriteByte(maxResolution);
    }
}

bool ExtraInitAck::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.TryRead(peerVersion) &&
           in.TryRead(minVersion) &&
           in.TryRead(streams, ver);
}
void ExtraInitAck::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteUInt32(peerVersion);
    out.WriteUInt32(minVersion);
    out.Write(streams, ver);
}

bool ExtraPong::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.Remaining() >= 4 ? in.TryRead(seq) : true;
}

void ExtraPong::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteUInt32(seq);
}

bool ExtraNetworkChanged::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.TryRead(streamId) &&
                   ver.isNew()
               ? in.TryRead(flags)
               : in.TryReadCompat<uint32_t>(flags);
}
void ExtraNetworkChanged::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteByte(streamId);
    ver.isNew() ? out.WriteByte(flags) : out.WriteUInt32(flags);
}
