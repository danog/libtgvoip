#include "Extra.h"

using namespace tgvoip;

std::shared_ptr<Extra> Extra::choose(const BufferInputStream &in, VersionInfo &ver)
{
    switch (in.ReadByte())
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

bool StreamInfo::parse(const BufferInputStream &in, VersionInfo &ver)
{
    streamId = in.ReadByte();
    type = static_cast<StreamType>(in.ReadByte());

    if (ver.peerVersion < 5) {
        if (in.ReadByte() == CODEC_OPUS_OLD) {
            codec = CODEC_OPUS;
        }
    } else {
        codec = in.ReadUInt32();
    }

    frameDuration = in.ReadUInt16();
    enabled = in.ReadByte();
}