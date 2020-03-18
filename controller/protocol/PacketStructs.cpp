#include "PacketStructs.h"
#include "../PrivateDefines.cpp"

using namespace tgvoip;

bool Packet::parse(const BufferInputStream &in, const VersionInfo &ver)
{
    if (!ver.isNew())
    {
        return parseLegacyPacket(in, ver);
    }
}

bool Packet::parseLegacyPacket(const BufferInputStream &in, const VersionInfo &ver)
{
    // Version-specific extraction of legacy packet fields ackId (last received packet seq on remote), (incoming packet seq) pseq, (ack mask) acks, (packet type) type, (flags) pflags, packet length
    uint32_t ackId;             // Last received packet seqno on remote
    uint32_t pseq;              // Incoming packet seqno
    uint32_t acks;              // Ack mask
    unsigned char type, pflags; // Packet type, flags
    size_t packetInnerLen = 0;
    if (peerVersion >= 8)
    {
        type = in.ReadByte();
        ackId = in.ReadUInt32();
        pseq = in.ReadUInt32();
        acks = in.ReadUInt32();
        pflags = in.ReadByte();
        packetInnerLen = in.Remaining();
    }
    else if (!legacyParsePacket(in, type, ackId, pseq, acks, pflags, packetInnerLen, peerVersion))
    {
        return false;
    }
    this->seq = pseq;
    this->ackSeq = ackId;
    this->ackMask = acks;

    // Extra data
    if (pflags & XPFLAG_HAS_EXTRA)
    {
        extras.parse(in, peerVersion);
    }
    if (pflags & XPFLAG_HAS_RECV_TS)
    {
        recvTS = in.ReadUInt32();
    }

    if (type == PKT_INIT)
    {
        Buffer
    }
    else if (type == PKT_INIT_ACK)
    {
    }
    else if (type == PKT_PING)
    {
    }
    else if (type == PKT_PONG)
    {
    }
    else if (type == PKT_NOP)
    {
    }
}
bool legacyParsePacket(const BufferInputStream &in, unsigned char &type, uint32_t &ackId, uint32_t &pseq, uint32_t &acks, unsigned char &pflags, size_t &packetInnerLen, int peerVersion)
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