#include "Extra.h"

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
    switch (id)
    {
    case ExtraStreamFlags::ID:
        return std::make_shared<ExtraStreamFlags>();
    case ExtraStreamCsd::ID:
        return std::make_shared<ExtraStreamCsd>();
    case ExtraLanEndpoint::ID:
        return std::make_shared<ExtraLanEndpoint>();
    case ExtraIpv6Endpoint::ID:
        return std::make_shared<ExtraIpv6Endpoint>();
    case ExtraNetworkChanged::ID:
        return std::make_shared<ExtraNetworkChanged>();
    case ExtraGroupCallKey::ID:
        return std::make_shared<ExtraGroupCallKey>();
    case ExtraGroupCallUpgrade::ID:
        return std::make_shared<ExtraGroupCallUpgrade>();
    }
}

bool StreamInfo::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    return in.TryRead(streamId) &&
           in.TryReadCompat<uint8_t>(type) &&
           in.TryRead(codec, ver) &&
           in.TryRead(frameDuration) &&
           in.TryReadCompat<uint8_t>(enabled);
}

void StreamInfo::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteByte(streamId);
    out.WriteByte(type);
    out.Write(codec, ver);
    out.WriteUInt16(frameDuration);
    out.WriteByte(enabled ? 1 : 0);
}

bool ExtraStreamFlags::parse(const BufferInputStream &in, const VersionInfo &ver)
{
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
    else
        out.WriteUInt32(flags);
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
           in.TryRead(port);
}
void ExtraLanEndpoint::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteUInt32(address.addr.ipv4);
    out.WriteUInt16(port);
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
    return in.TryRead(key.data(), key.size());
}
void ExtraGroupCallKey::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    out.WriteBytes(key.data(), key.size());
}

bool ExtraInit::parse(const BufferInputStream &in, const VersionInfo &ver)
{
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
        out.WriteByte(0); // second useless codec
        out.WriteByte(0); // what is this
        out.WriteByte(0); // what is this
        out.WriteInt32(CODEC_OPUS); // WHAT IS THIS
        out.WriteByte(0); // video codecs count (decode)
        out.WriteByte(0); // video codecs count (encode)
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
