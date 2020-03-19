#include "PacketStructs.h"
#include "../PrivateDefines.cpp"

using namespace tgvoip;

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
void Packet::serializeLegacy(BufferOutputStream &out, const VersionInfo &ver) const
{
    uint8_t type = PKT_NOP;
    for (const auto &extra : extraSignaling) {
        
    }
    if (ver.peerVersion >= 8 || (!ver.peerVersion && ver.connectionMaxLayer >= 92))
    {
        out.WriteByte(pkt.type);
        out.WriteInt32(manager.getLastRemoteSeq());
        out.WriteInt32(pkt.seq);
        out.WriteInt32(acks);

        unsigned char flags = currentExtras.empty() ? 0 : XPFLAG_HAS_EXTRA;

        shared_ptr<Stream> videoStream = GetStreamByType(STREAM_TYPE_VIDEO, false);
        if (peerVersion >= 9 && videoStream && videoStream->enabled)
            flags |= XPFLAG_HAS_RECV_TS;

        if (peerVersion >= PROTOCOL_RELIABLE && manager.getTransportId() != 0xFF)
            flags |= XPFLAG_HAS_TRANSPORT_ID;

        out.WriteByte(flags);

        if (!currentExtras.empty())
        {
            out.WriteByte(static_cast<unsigned char>(currentExtras.size()));
            for (auto &x : currentExtras)
            {
                //LOGV("Writing extra into header: type %u, length %d", x.type, int(x.data.Length()));
                assert(x.data.Length() <= 254);
                out.WriteByte(static_cast<unsigned char>(x.data.Length() + 1));
                out.WriteByte(x.type);
                out.WriteBytes(*x.data, x.data.Length());
                if (x.firstContainingSeq == 0)
                    x.firstContainingSeq = pkt.seq;
            }
        }
        if (peerVersion >= 9 && videoStream && videoStream->enabled)
        {
            out.WriteUInt32((lastRecvPacketTime - connectionInitTime) * 1000.0);
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

void Packet::serializeLegacyLegacy(BufferOutputStream &out, uint32_t pseq, uint32_t acks, unsigned char type, uint32_t length) const
{
    if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
    {
        out.WriteInt32(TLID_DECRYPTED_AUDIO_BLOCK);
        int64_t randomID;
        crypto.rand_bytes((uint8_t *)&randomID, 8);
        out.WriteInt64(randomID);
        unsigned char randBytes[7];
        crypto.rand_bytes(randBytes, 7);
        out.WriteByte(7);
        out.WriteBytes(randBytes, 7);
        uint32_t pflags = LEGACY_PFLAG_HAS_RECENT_RECV | LEGACY_PFLAG_HAS_SEQ;
        if (length > 0)
            pflags |= LEGACY_PFLAG_HAS_DATA;
        if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
        {
            pflags |= LEGACY_PFLAG_HAS_CALL_ID | LEGACY_PFLAG_HAS_PROTO;
        }
        pflags |= ((uint32_t)type) << 24;
        out.WriteInt32(pflags);

        if (pflags & LEGACY_PFLAG_HAS_CALL_ID)
        {
            out.WriteBytes(callID, 16);
        }
        out.WriteInt32(packetManager.getLastRemoteSeq());
        out.WriteInt32(pseq);
        out.WriteInt32(acks);
        if (pflags & LEGACY_PFLAG_HAS_PROTO)
        {
            out.WriteInt32(PROTOCOL_NAME);
        }
        if (length > 0)
        {
            if (length <= 253)
            {
                out.WriteByte((unsigned char)length);
            }
            else
            {
                out.WriteByte(254);
                out.WriteByte((unsigned char)(length & 0xFF));
                out.WriteByte((unsigned char)((length >> 8) & 0xFF));
                out.WriteByte((unsigned char)((length >> 16) & 0xFF));
            }
        }
    }
    else
    {
        out.WriteInt32(TLID_SIMPLE_AUDIO_BLOCK);
        int64_t randomID;
        crypto.rand_bytes((uint8_t *)&randomID, 8);
        out.WriteInt64(randomID);
        unsigned char randBytes[7];
        crypto.rand_bytes(randBytes, 7);
        out.WriteByte(7);
        out.WriteBytes(randBytes, 7);
        uint32_t lenWithHeader = length + 13;
        if (lenWithHeader > 0)
        {
            if (lenWithHeader <= 253)
            {
                out.WriteByte((unsigned char)lenWithHeader);
            }
            else
            {
                out.WriteByte(254);
                out.WriteByte((unsigned char)(lenWithHeader & 0xFF));
                out.WriteByte((unsigned char)((lenWithHeader >> 8) & 0xFF));
                out.WriteByte((unsigned char)((lenWithHeader >> 16) & 0xFF));
            }
        }
        out.WriteByte(type);
        out.WriteInt32(packetManager.getLastRemoteSeq());
        out.WriteInt32(pseq);
        out.WriteInt32(acks);
        if (peerVersion >= 6)
        {
            if (currentExtras.empty())
            {
                out.WriteByte(0);
            }
            else
            {
                out.WriteByte(XPFLAG_HAS_EXTRA);
                out.WriteByte(static_cast<unsigned char>(currentExtras.size()));
                for (vector<UnacknowledgedExtraData>::iterator x = currentExtras.begin(); x != currentExtras.end(); ++x)
                {
                    LOGV("Writing extra into header: type %u, length %d", x->type, int(x->data.Length()));
                    assert(x->data.Length() <= 254);
                    out.WriteByte(static_cast<unsigned char>(x->data.Length() + 1));
                    out.WriteByte(x->type);
                    out.WriteBytes(*x->data, x->data.Length());
                    if (x->firstContainingSeq == 0)
                        x->firstContainingSeq = pseq;
                }
            }
        }
    }
}
