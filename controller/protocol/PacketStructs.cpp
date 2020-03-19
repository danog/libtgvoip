#include "PacketStructs.h"
#include "../PrivateDefines.cpp"

using namespace tgvoip;

bool Packet::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    if (!ver.isNew())
    {
        legacy = true;
        return parseLegacy(in, ver);
    }

    uint16_t length;
    uint8_t flags;
    bool res = in.TryRead(seq) &&
               in.TryRead(ackSeq) &&
               in.TryRead(ackMask) &&
               in.TryRead(flags);
    if (!res)
        return false;

    streamId = flags & 3;
    flags >>= 2;

    if (streamId == StreamId::Extended && !in.TryRead(streamId))
        return false;
    if (!(flags & Flags::Len16 ? in.TryRead(length) : in.TryReadCompat<uint8_t>(length)))
        return false;

    eFlags = length >> 11;
    length &= 0x7FF;

    if (eFlags & EFlags::Fragmented)
    {
        if (!in.TryRead(fragmentCount))
            return false;
        if (!in.TryRead(fragmentIndex))
            return false;
    }

    data = Buffer(length);
    if (in.TryRead(data))
        return false;

    if ((flags & Flags::RecvTS) && !in.TryRead(recvTS))
        return false;
    if ((flags & Flags::ExtraEC) && !in.TryRead(extraEC, ver))
        return false;
    if ((flags & Flags::ExtraSignaling) && !in.TryRead(extraSignaling, ver))
        return false;

    return true;
}

void Packet::serialize(BufferOutputStream &out, const VersionInfo &ver) const
{
    if (!ver.isNew() || legacy)
    {
        return serializeLegacy(out, ver);
    }

    uint8_t shortStreamId = streamId > StreamId::Extended ? StreamId::Extended : streamId;
    uint8_t flags = 0;
    if (data.Length() > 0xFF || eFlags)
        flags |= Flags::Len16;
    if (recvTS)
        flags |= Flags::RecvTS;
    if (extraEC)
        flags |= Flags::ExtraEC;
    if (extraSignaling)
        flags |= Flags::ExtraSignaling;

    out.WriteUInt32(seq);
    out.WriteUInt32(ackSeq);
    out.WriteUInt32(ackMask);
    out.WriteByte(shortStreamId | (flags << 2));

    if (shortStreamId == StreamId::Extended)
        out.WriteByte(streamId);

    if (flags & Flags::Len16)
        out.WriteUInt16(data.Length() | (eFlags << 11));
    else
        out.WriteByte(data.Length());

    if (flags & Flags::RecvTS)
        out.WriteUInt32(recvTS);
    if (flags & Flags::ExtraEC)
        out.Write(extraEC, ver);
    if (flags & Flags::ExtraSignaling)
        out.Write(extraSignaling, ver);
}

#include "PacketStructsLegacy.cpp"