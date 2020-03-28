#include "../../../VoIPController.h"
#include "PacketStructs.h"

using namespace tgvoip;

bool Packet::parseLegacy(const BufferInputStream &in, const VersionInfo &ver)
{
    legacy = true;

    // Version-specific extraction of legacy packet fields ackId (last received packet seq on remote), (incoming packet seq) pseq, (ack mask) acks, (packet type) type, (flags) pflags, packet length
    uint32_t ackId;             // Last received packet seqno on remote
    uint32_t pseq;              // Incoming packet seqno
    uint32_t acks;              // Ack mask
    unsigned char type, pflags; // Packet type, flags
    size_t packetInnerLen = 0;
    if (ver.isLegacy())
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
            seq += 1;  // Account for seq starting at 1

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
        for (uint8_t count = type == PKT_STREAM_DATA ? 1 : type == PKT_STREAM_DATA_X2 ? 2 : 3; count > 0; --count)
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
            if (!(in.TryRead(flags) &&
                  (flags & STREAM_DATA_FLAG_LEN16
                       ? in.TryRead(len)
                       : in.TryReadCompat<uint8_t>(len)) &&
                  in.TryRead(packet->seq)))
                return false;

            packet->streamId = flags & 0x3F;
            packet->seq /= 60; // Constant frame duration
            packet->seq += 1; // Account for seqs starting at 1

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

            packet->data = std::make_unique<Buffer>(len & 0x7FF);
            if (!in.TryRead(*packet->data))
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
    else
    {
        LOGW("Got unknown legacy packet type %hhu (probably new packet)", type);
        return false;
    }
    return true;
}

void Packet::serializeLegacy(std::vector<std::pair<Buffer, bool>> &outArray, const VersionInfo &ver, const int state, const unsigned char *callID)
{
    auto originalLegacySeq = legacySeq;

    std::vector<Wrapped<Extra>> separatePackets;
    std::vector<Wrapped<Extra>> allowedExtras;
    std::partition_copy(
        std::make_move_iterator(extraSignaling.begin()),
        std::make_move_iterator(extraSignaling.end()),
        std::back_inserter(separatePackets),
        std::back_inserter(allowedExtras),
        [peerVersion = ver.peerVersion](const Wrapped<Extra> &extra) { // Deliver as packet if
            return extra.d->chooseType(peerVersion) != PKT_NOP;
        });

    for (const auto &extra : separatePackets)
    {
        BufferOutputStream out(1500);

        uint8_t type = extra.d->chooseType(ver.peerVersion);

#ifdef LOG_PACKETS
        LOGW("Serializing separate legacy packet of type %s", VoIPController::GetPacketTypeString(type).c_str());
#endif

        if (ver.peerVersion >= 8 || (!ver.peerVersion && ver.connectionMaxLayer >= 92))
        {
            writePacketHeaderLegacy(out, ver, legacySeq, ackSeq, ackMask, type, allowedExtras);
            out.Write(*extra.d, ver);
        }
        else
        {
            BufferOutputStream accumulator(1500);
            accumulator.Write(*extra.d, ver);
            writePacketHeaderLegacyLegacy(out, ver, legacySeq, ackSeq, ackMask, type, accumulator.GetLength(), allowedExtras, state, callID);
            out.WriteBytes(accumulator.GetBuffer(), accumulator.GetLength());
        }
        outArray.push_back(std::make_pair(Buffer(std::move(out)), true));
        legacySeq++;
    }
    // Convert from mask to array
    Array<Wrapped<Bytes>> extraECArray;
    if (extraEC)
    {
        for (auto &ecPacket : extraEC.v)
        {
            extraECArray.v.push_back(std::move(ecPacket));
        }
    }

    if (ver.peerVersion < 7 && extraECArray)
    {
        LOGW("Serializing legacylegacy EC");
        BufferOutputStream out(1500);

        if (!ver.isLegacyLegacy())
        {
            writePacketHeaderLegacy(out, ver, legacySeq, ackSeq, ackMask, PKT_STREAM_EC, allowedExtras);
        }
        out.WriteByte(streamId);
        out.WriteUInt32((seq - 1) * 60); // Account for seq starting at 1
        out.Write(extraECArray, ver);

        if (ver.isLegacyLegacy())
        {
            BufferOutputStream accumulator(1500);

            writePacketHeaderLegacyLegacy(accumulator, ver, legacySeq, ackSeq, ackMask, PKT_STREAM_EC, out.GetLength(), allowedExtras, state, callID);
            accumulator.WriteBytes(out.GetBuffer(), out.GetLength());

            outArray.push_back(std::make_pair(Buffer(std::move(accumulator)), false));
        }
        else
        {
            outArray.push_back(std::make_pair(Buffer(std::move(out)), false));
        }

        legacySeq++;
    }
    if (data)
    {
        //LOGW("Serializing legacy data len %u", (unsigned int)data->Length());

        BufferOutputStream out(1500);

        if (!ver.isLegacyLegacy())
        {
            writePacketHeaderLegacy(out, ver, legacySeq, ackSeq, ackMask, PKT_STREAM_DATA, allowedExtras);
        }

        bool hasExtraFEC = ver.peerVersion >= 7 && extraECArray;
        uint8_t flags = static_cast<uint8_t>(data->Length() > 255 || hasExtraFEC ? STREAM_DATA_FLAG_LEN16 : 0);
        out.WriteByte(flags | 1); // flags + streamID

        if (flags & STREAM_DATA_FLAG_LEN16)
        {
            int16_t lenAndFlags = static_cast<int16_t>(data->Length());
            if (hasExtraFEC)
                lenAndFlags |= STREAM_DATA_XFLAG_EXTRA_FEC;
            out.WriteInt16(lenAndFlags);
        }
        else
        {
            out.WriteByte(static_cast<uint8_t>(data->Length()));
        }

        out.WriteInt32((seq - 1) * 60); // Account for seq starting at 1
        out.WriteBytes(*data);

        if (hasExtraFEC)
        {
            out.Write(extraECArray, ver);
        }

        if (ver.isLegacyLegacy())
        {
            BufferOutputStream accumulator(1500);

            writePacketHeaderLegacyLegacy(accumulator, ver, legacySeq, ackSeq, ackMask, PKT_STREAM_DATA, out.GetLength(), allowedExtras, state, callID);
            accumulator.WriteBytes(out.GetBuffer(), out.GetLength());

            outArray.push_back(std::make_pair(Buffer(std::move(accumulator)), false));
        }
        else
        {
            outArray.push_back(std::make_pair(Buffer(std::move(out)), false));
        }

        legacySeq++;
    }
    if (legacySeq == originalLegacySeq) // No data was serialized
    {
        LOGW("Serializing legacy NOP packet");
        BufferOutputStream out(1500);

        if (ver.peerVersion >= 8 || (!ver.peerVersion && ver.connectionMaxLayer >= 92))
        {
            writePacketHeaderLegacy(out, ver, legacySeq, ackSeq, ackMask, PKT_NOP, allowedExtras);
        }
        else
        {
            writePacketHeaderLegacyLegacy(out, ver, legacySeq, ackSeq, ackMask, PKT_NOP, 0, allowedExtras, state, callID);
        }
        outArray.push_back(std::make_pair(Buffer(std::move(out)), false));
        legacySeq++;
    }
}
void Packet::writePacketHeaderLegacy(BufferOutputStream &out, const VersionInfo &ver, const uint32_t seq, const uint32_t ackSeq, const uint32_t ackMask, const unsigned char type, const std::vector<Wrapped<Extra>> &extras)
{
    out.WriteByte(type);
    out.WriteInt32(ackSeq);
    out.WriteInt32(seq);
    out.WriteInt32(ackMask);

    unsigned char flags = extras.empty() ? 0 : XPFLAG_HAS_EXTRA;

    if (recvTS)
        flags |= XPFLAG_HAS_RECV_TS;

    out.WriteByte(flags);

    if (!extras.empty())
    {
        out.WriteByte(static_cast<unsigned char>(extras.size()));
        for (auto &x : extras)
        {
            LOGV("Writing extra into header: type %u", x.getID());
            out.Write(x, ver);
        }
    }
    if (recvTS)
    {
        out.WriteUInt32(recvTS);
    }
}
bool Packet::parseLegacyLegacy(const BufferInputStream &in, unsigned char &type, uint32_t &ackId, uint32_t &pseq, uint32_t &acks, unsigned char &pflags, size_t &packetInnerLen, int peerVersion)
{
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

void Packet::writePacketHeaderLegacyLegacy(BufferOutputStream &out, const VersionInfo &ver, const uint32_t pseq, const uint32_t ackSeq, const uint32_t ackMask, const unsigned char type, const uint32_t length, const std::vector<Wrapped<Extra>> &extras, const int state, const unsigned char *callID)
{
    out.WriteInt32(state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK ? TLID_DECRYPTED_AUDIO_BLOCK : TLID_SIMPLE_AUDIO_BLOCK);
    int64_t randomID;
    VoIPController::crypto.rand_bytes((uint8_t *)&randomID, 8);
    out.WriteInt64(randomID);
    unsigned char randBytes[7];
    VoIPController::crypto.rand_bytes(randBytes, 7);
    out.WriteByte(7);
    out.WriteBytes(randBytes, 7);

    if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
    {
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
        out.WriteInt32(ackSeq);
        out.WriteInt32(pseq);
        out.WriteInt32(ackMask);
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
        out.WriteInt32(ackSeq);
        out.WriteInt32(pseq);
        out.WriteInt32(ackMask);
        if (ver.peerVersion >= 6)
        {
            if (extras.empty())
            {
                out.WriteByte(0);
            }
            else
            {
                out.WriteByte(XPFLAG_HAS_EXTRA);
                out.WriteByte(static_cast<unsigned char>(extras.size()));
                for (auto &x : extras)
                {
                    //LOGV("Writing extra into header: type %u", x.getID());
                    out.Write(x, ver);
                }
            }
        }
    }
}
