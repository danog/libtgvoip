#include "../PrivateDefines.cpp"

using namespace tgvoip;
using namespace std;


void VoIPController::legacyWritePacketHeader(uint32_t pseq, uint32_t acks, BufferOutputStream *s, unsigned char type, uint32_t length)
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
        uint32_t pflags = LEGACY_PFLAG_HAS_RECENT_RECV | LEGACY_PFLAG_HAS_SEQ;
        if (length > 0)
            pflags |= LEGACY_PFLAG_HAS_DATA;
        if (state == STATE_WAIT_INIT || state == STATE_WAIT_INIT_ACK)
        {
            pflags |= LEGACY_PFLAG_HAS_CALL_ID | LEGACY_PFLAG_HAS_PROTO;
        }
        pflags |= ((uint32_t)type) << 24;
        s->WriteInt32(pflags);

        if (pflags & LEGACY_PFLAG_HAS_CALL_ID)
        {
            s->WriteBytes(callID, 16);
        }
        s->WriteInt32(packetManager.getLastRemoteSeq());
        s->WriteInt32(pseq);
        s->WriteInt32(acks);
        if (pflags & LEGACY_PFLAG_HAS_PROTO)
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
        s->WriteInt32(packetManager.getLastRemoteSeq());
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
