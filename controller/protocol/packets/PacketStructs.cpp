#include "PacketStructs.h"
#include "../../../VoIPController.h"
#include "PacketManager.h"

using namespace tgvoip;

bool Packet::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    if (!ver.isNew())
    {
        return parseLegacy(in, ver);
    }

    uint16_t length = 0;
    uint8_t flags;
    bool res = in.TryRead(flags) &&
               in.TryRead(seq) &&
               in.TryRead(ackSeq) &&
               in.TryRead(ackMask);
    if (!res)
        return false;

    streamId = flags & 3;
    flags >>= 2;

    if (!(flags & Flags::Reserved))
    {
        LOGW("No reserved flag in new flag, probably legacy packet");
        return false;
    }
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

    if (length)
    {
        data = std::make_unique<Buffer>(length);
        if (!in.TryRead(*data))
            return false;
    }

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
    uint8_t shortStreamId = streamId > StreamId::Extended ? StreamId::Extended : streamId;
    uint8_t flags = Flags::Reserved; // Always true, allows us to distinguish from legacy packets if the maxLayer is wrong.
    size_t length = data ? data->Length() : 0;

    if (length > 0xFF || eFlags)
        flags |= Flags::Len16;
    if (recvTS)
        flags |= Flags::RecvTS;
    if (extraEC)
        flags |= Flags::ExtraEC;
    if (extraSignaling)
        flags |= Flags::ExtraSignaling;

    out.WriteByte(shortStreamId | (flags << 2));
    out.WriteUInt32(seq);
    out.WriteUInt32(ackSeq);
    out.WriteUInt32(ackMask);

    if (shortStreamId == StreamId::Extended)
        out.WriteByte(streamId);

    if (flags & Flags::Len16)
        out.WriteUInt16(length | (eFlags << 11));
    else
        out.WriteByte(length);

    if (length)
        out.WriteBytes(*data);

    if (flags & Flags::RecvTS)
        out.WriteUInt32(recvTS);
    if (flags & Flags::ExtraEC)
        out.Write(extraEC, ver);
    if (flags & Flags::ExtraSignaling)
        out.Write(extraSignaling, ver);
}

void Packet::prepare(PacketManager &pm)
{
    if (!seq)
    {
        seq = pm.nextLocalSeq();
        ackSeq = pm.getLastRemoteSeq();
        ackMask = pm.getRemoteAckMask();
        streamId = pm.transportId;
    }
}
void Packet::prepare(PacketManager &pm, std::vector<UnacknowledgedExtraData> &currentExtras, const int64_t &endpointId)
{
    prepare(pm);
    extraSignaling.v.clear();
    for (auto &extra : currentExtras)
    {
        if (!extra.endpointId || extra.endpointId == endpointId)
        {
            extraSignaling.v.push_back(extra.data);
            extra.seqs.Add(seq);
        }
    }
}
void Packet::prepare(PacketManager &pm, std::vector<UnacknowledgedExtraData> &currentExtras, const int64_t &endpointId, PacketManager &legacyPm, const int peerVersion)
{
    prepare(pm);
    if (!legacySeq)
    {
        if (pm != legacyPm)
        {
            legacySeq = legacyPm.nextLocalSeq();
            ackSeq = legacyPm.getLastRemoteSeq();
            ackMask = legacyPm.getRemoteAckMask();
        }
        else
        {
            legacySeq = seq;
        }
    }

    int tmpLegacySeq = legacySeq;

    extraSignaling.v.clear();
    for (auto &extra : currentExtras)
    {
        if (!extra.endpointId || extra.endpointId == endpointId)
        {
            extraSignaling.v.push_back(extra.data);
            if (extra.data.d->chooseType(peerVersion) == PKT_NOP)
            {
#ifdef LOG_PACKETS
                LOGW("Adding legacy seq %u to extra %s", seq, extra.data.print().c_str());
#endif
                extra.seqs.Add(seq);
            }
            else
            {
#ifdef LOG_PACKETS
                LOGW("Adding legacy seq %u to extra %s", tmpLegacySeq, extra.data.print().c_str());
#endif
                extra.seqs.Add(tmpLegacySeq++);
            }
        }
    }
}
#include "Legacy.cpp"