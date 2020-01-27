#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;

bool VoIPController::legacyParsePacket(BufferInputStream &in, unsigned char &type, uint32_t &ackId, uint32_t &pseq, uint32_t &acks, unsigned char &pflags, size_t &packetInnerLen)
{
    uint32_t tlid = in.ReadUInt32();
    if (tlid == TLID_DECRYPTED_AUDIO_BLOCK)
    {
        in.ReadInt64(); // random id
        uint32_t randLen = in.ReadTlLength();
        in.Seek(in.GetOffset() + randLen + pad4(randLen));
        uint32_t flags = in.ReadUInt32();
        type = (unsigned char)((flags >> 24) & 0xFF);
        if (!(flags & PFLAG_HAS_SEQ && flags & PFLAG_HAS_RECENT_RECV))
        {
            LOGW("Received packet doesn't have PFLAG_HAS_SEQ, PFLAG_HAS_RECENT_RECV, or both");

            return false;
        }
        if (flags & PFLAG_HAS_CALL_ID)
        {
            unsigned char pktCallID[16];
            in.ReadBytes(pktCallID, 16);
            if (memcmp(pktCallID, callID, 16) != 0)
            {
                LOGW("Received packet has wrong call id");

                lastError = ERROR_UNKNOWN;
                SetState(STATE_FAILED);
                return false;
            }
        }
        ackId = in.ReadUInt32();
        pseq = in.ReadUInt32();
        acks = in.ReadUInt32();
        if (flags & PFLAG_HAS_PROTO)
        {
            uint32_t proto = in.ReadUInt32();
            if (proto != PROTOCOL_NAME)
            {
                LOGW("Received packet uses wrong protocol");

                lastError = ERROR_INCOMPATIBLE;
                SetState(STATE_FAILED);
                return false;
            }
        }
        if (flags & PFLAG_HAS_EXTRA)
        {
            uint32_t extraLen = in.ReadTlLength();
            in.Seek(in.GetOffset() + extraLen + pad4(extraLen));
        }
        if (flags & PFLAG_HAS_DATA)
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

void VoIPController::legacyWritePacketHeader(uint32_t pseq, uint32_t acks, BufferOutputStream *s, unsigned char type, uint32_t length, PacketSender *source)
{

    if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
    {
        s->WriteInt32(TLID_DECRYPTED_AUDIO_BLOCK);
        int64_t randomID;
        crypto.rand_bytes((uint8_t *)&randomID, 8);
        s->WriteInt64(randomID);
        unsigned char randBytes[7];
        crypto.rand_bytes(randBytes, 7);
        s->WriteByte(7);
        s->WriteBytes(randBytes, 7);
        uint32_t pflags = PFLAG_HAS_RECENT_RECV | PFLAG_HAS_SEQ;
        if (length > 0)
            pflags |= PFLAG_HAS_DATA;
        if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
        {
            pflags |= PFLAG_HAS_CALL_ID | PFLAG_HAS_PROTO;
        }
        pflags |= ((uint32_t)type) << 24;
        s->WriteInt32(pflags);

        if (pflags & PFLAG_HAS_CALL_ID)
        {
            s->WriteBytes(callID, 16);
        }
        s->WriteInt32(lastRemoteSeq);
        s->WriteInt32(pseq);
        s->WriteInt32(acks);
        if (pflags & PFLAG_HAS_PROTO)
        {
            s->WriteInt32(PROTOCOL_NAME);
        }
        if (length > 0)
        {
            if (length <= 253)
            {
                s->WriteByte((unsigned char)length);
            }
            else
            {
                s->WriteByte(254);
                s->WriteByte((unsigned char)(length & 0xFF));
                s->WriteByte((unsigned char)((length >> 8) & 0xFF));
                s->WriteByte((unsigned char)((length >> 16) & 0xFF));
            }
        }
    }
    else
    {
        s->WriteInt32(TLID_SIMPLE_AUDIO_BLOCK);
        int64_t randomID;
        crypto.rand_bytes((uint8_t *)&randomID, 8);
        s->WriteInt64(randomID);
        unsigned char randBytes[7];
        crypto.rand_bytes(randBytes, 7);
        s->WriteByte(7);
        s->WriteBytes(randBytes, 7);
        uint32_t lenWithHeader = length + 13;
        if (lenWithHeader > 0)
        {
            if (lenWithHeader <= 253)
            {
                s->WriteByte((unsigned char)lenWithHeader);
            }
            else
            {
                s->WriteByte(254);
                s->WriteByte((unsigned char)(lenWithHeader & 0xFF));
                s->WriteByte((unsigned char)((lenWithHeader >> 8) & 0xFF));
                s->WriteByte((unsigned char)((lenWithHeader >> 16) & 0xFF));
            }
        }
        s->WriteByte(type);
        s->WriteInt32(lastRemoteSeq);
        s->WriteInt32(pseq);
        s->WriteInt32(acks);
        if (peerVersion >= 6)
        {
            if (currentExtras.empty())
            {
                s->WriteByte(0);
            }
            else
            {
                s->WriteByte(XPFLAG_HAS_EXTRA);
                s->WriteByte(static_cast<unsigned char>(currentExtras.size()));
                for (vector<UnacknowledgedExtraData>::iterator x = currentExtras.begin(); x != currentExtras.end(); ++x)
                {
                    LOGV("Writing extra into header: type %u, length %d", x->type, int(x->data.Length()));
                    assert(x->data.Length() <= 254);
                    s->WriteByte(static_cast<unsigned char>(x->data.Length() + 1));
                    s->WriteByte(x->type);
                    s->WriteBytes(*x->data, x->data.Length());
                    if (x->firstContainingSeq == 0)
                        x->firstContainingSeq = pseq;
                }
            }
        }
    }
}
