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

    bool res = in.TryRead(seq) &&
               in.TryRead(ackSeq) &&
               in.TryRead(ackMask) &&
               in.TryRead(flags);
    if (!res)
        return false;

    streamId = flags & 3;
    flags >>= 2;

    uint16_t length;
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
    if ((flags & Flags::ExtraFEC) && !in.TryRead(extraEC, ver))
        return false;
    if ((flags & Flags::ExtraSignaling) && !in.TryRead(extraSignaling, ver))
        return false;
    
    return true;
}

bool Packet::parseLegacy(const BufferInputStream &in, const VersionInfo &ver)
{
    // Version-specific extraction of legacy packet fields ackId (last received packet seq on remote), (incoming packet seq) pseq, (ack mask) acks, (packet type) type, (flags) pflags, packet length
    uint32_t ackId;             // Last received packet seqno on remote
    uint32_t pseq;              // Incoming packet seqno
    uint32_t acks;              // Ack mask
    unsigned char type, pflags; // Packet type, flags
    size_t packetInnerLen = 0;
    if (ver.peerVersion >= 8)
    {
        if (!(
                in.TryRead(type) &&
                in.TryRead(ackId) &&
                in.TryRead(pseq) &&
                in.TryRead(acks) &&
                in.TryRead(pflags)))
            return false;
        packetInnerLen = in.Remaining();
    }
    else if (!parseLegacyLegacy(in, type, ackId, pseq, acks, pflags, packetInnerLen, ver.peerVersion))
    {
        return false;
    }
    this->legacySeq = pseq;
    this->ackSeq = ackId;
    this->ackMask = acks;

    // Extra data
    if (pflags & XPFLAG_HAS_EXTRA)
    {
        if (!in.TryRead(extraSignaling, ver))
            return false;
    }
    if (pflags & XPFLAG_HAS_RECV_TS)
    {
        if (!in.TryRead(recvTS))
            return false;
    }

    if (auto extra = Extra::chooseFromType(type))
    {
        if (!extra->parse(in, ver))
            return false;

        extraSignaling.v.push_back(Wrapped<Extra>(std::move(extra)));
        streamId = 0;
    }
    else if (type == PKT_STREAM_EC)
    {
        if (ver.peerVersion < 7)
        {
            uint8_t count;
            if (!(in.TryRead(streamId) && in.TryRead(seq) && in.TryRead(count)))
                return false;

            seq /= 60; // Constant frame duration

            for (uint8_t i = 0; i < std::min(count, uint8_t{8}); i++)
            {
                if (!in.TryRead(extraEC.v[i], ver))
                    return false;
            }
            for (uint8_t i = count > 8 ? 8 - count : 0; i > 0; i--)
            {
                Wrapped<Bytes> ignored;
                if (!in.TryRead(ignored, ver))
                    return false;
            }
        }
        else
        {
            // Ignore video FEC for now
        }
    }
    else if (type == PKT_STREAM_DATA || type == PKT_STREAM_DATA_X2 || type == PKT_STREAM_DATA_X3)
    {
        for (uint8_t count = PKT_STREAM_DATA ? 1 : PKT_STREAM_DATA_X2 ? 2 : 3; count > 0; --count)
        {
            Packet *packet = this;
            if (count > 1)
            {
                otherPackets.push_back(Packet());
                packet = &otherPackets.back();
                packet->legacy = true;
            }

            uint8_t flags;
            uint16_t len;
            if (!(in.TryRead(packet->streamId) &&
                          in.TryRead(flags) &&
                          flags & STREAM_DATA_FLAG_LEN16
                      ? in.TryRead(len)
                      : in.TryReadCompat<uint8_t>(len) &&
                            in.TryRead(packet->seq))) // damn you autoindentation
                return false;

            packet->seq /= 60; // Constant frame duration

            bool fragmented = static_cast<bool>(len & STREAM_DATA_XFLAG_FRAGMENTED);
            bool extraFEC = static_cast<bool>(len & STREAM_DATA_XFLAG_EXTRA_FEC);
            bool keyframe = static_cast<bool>(len & STREAM_DATA_XFLAG_KEYFRAME);
            if (fragmented)
            {
                packet->eFlags |= EFlags::Fragmented;
                if (!in.TryRead(packet->fragmentIndex))
                    return false;
                if (!in.TryRead(packet->fragmentCount))
                    return false;
            }
            if (keyframe)
            {
                packet->eFlags |= EFlags::Keyframe;
            }

            packet->data = Buffer(len & 0x7FF);
            if (!in.TryRead(packet->data))
                return false;

            if (extraFEC)
            {
                uint8_t count;
                if (!in.TryRead(count))
                    return false;
                for (uint8_t i = 0; i < std::min(count, uint8_t{8}); i++)
                {
                    if (!in.TryRead(packet->extraEC.v[i], ver))
                        return false;
                }
                for (uint8_t i = count > 8 ? 8 - count : 0; i > 0; i--)
                {
                    Wrapped<Bytes> ignored;
                    if (!in.TryRead(ignored, ver))
                        return false;
                }
            }
        }
    }
}
bool Packet::parseLegacyLegacy(const BufferInputStream &in, unsigned char &type, uint32_t &ackId, uint32_t &pseq, uint32_t &acks, unsigned char &pflags, size_t &packetInnerLen, int peerVersion)
{
    size_t packetInnerLen = 0;
    uint32_t tlid = in.ReadUInt32();
    if (tlid == TLID_DECRYPTED_AUDIO_BLOCK)
    {
        in.ReadInt64(); // random id
        uint32_t randLen = in.ReadTlLength();
        in.Seek(in.GetOffset() + randLen + pad4(randLen));
        uint32_t flags = in.ReadUInt32();
        type = (unsigned char)((flags >> 24) & 0xFF);
        if (!(flags & LEGACY_PFLAG_HAS_SEQ && flags & LEGACY_PFLAG_HAS_RECENT_RECV))
        {
            LOGW("Received packet doesn't have LEGACY_PFLAG_HAS_SEQ, LEGACY_PFLAG_HAS_RECENT_RECV, or both");

            return false;
        }
        // These are all errors that weren't even used in the first place in newer protocols.
        // Also, a call cannot possibly have a wrong call ID and correct encryption key (unless there's some shady legacy stuff that's going on, but for now I won't even consider it).
        /*
        if (flags & LEGACY_PFLAG_HAS_CALL_ID)
        {
            unsigned char pktCallID[16];
            in.ReadBytes(pktCallID, 16);
            if (memcmp(pktCallID, callID, 16) != 0)
            {
                LOGW("Received packet has wrong call id");

                // These are all errors that weren't even used in the first place in newer protocols
                //lastError = ERROR_UNKNOWN;
                //SetState(STATE_FAILED);

                return false;
            }
        }*/
        ackId = in.ReadUInt32();
        pseq = in.ReadUInt32();
        acks = in.ReadUInt32();
        if (flags & LEGACY_PFLAG_HAS_PROTO)
        {
            uint32_t proto = in.ReadUInt32();
            if (proto != PROTOCOL_NAME)
            {
                LOGW("Received packet uses wrong protocol");

                // These are all errors that weren't even used in the first place in newer protocols
                //lastError = ERROR_INCOMPATIBLE;
                //SetState(STATE_FAILED);
                return false;
            }
        }
        if (flags & LEGACY_PFLAG_HAS_EXTRA)
        {
            uint32_t extraLen = in.ReadTlLength();
            in.Seek(in.GetOffset() + extraLen + pad4(extraLen));
        }
        if (flags & LEGACY_PFLAG_HAS_DATA)
        {
            packetInnerLen = in.ReadTlLength();
        }
        pflags = 0;
    }
    else if (tlid == TLID_SIMPLE_AUDIO_BLOCK)
    {
        in.ReadInt64(); // random id
        uint32_t randLen = in.ReadTlLength();
        in.Seek(in.GetOffset() + randLen + pad4(randLen));
        packetInnerLen = in.ReadTlLength();
        type = in.ReadByte();
        ackId = in.ReadUInt32();
        pseq = in.ReadUInt32();
        acks = in.ReadUInt32();
        if (peerVersion >= 6)
            pflags = in.ReadByte();
        else
            pflags = 0;
    }
    else
    {
        LOGW("Received a packet of unknown type %08X", tlid);

        return false;
    }
    return true;
}