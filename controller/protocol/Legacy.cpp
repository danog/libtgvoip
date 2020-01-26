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
void VoIPController::legacyHandleQueuedPackets()
{
    for (auto it = queuedPackets.begin(); it != queuedPackets.end();)
    {
        QueuedPacket &qp = *it;
        bool didAck = false;
        for (uint8_t j = 0; j < 16; j++)
        {
            LOGD("queued packet %ld, seq %u=%u", queuedPackets.end() - it, j, qp.seqs[j]);
            if (qp.seqs[j] == 0)
                break;
            int remoteAcksIndex = lastRemoteAckSeq - qp.seqs[j];
            //LOGV("remote acks index %u, value %f", remoteAcksIndex, remoteAcksIndex>=0 && remoteAcksIndex<32 ? remoteAcks[remoteAcksIndex] : -1);
            if (seqgt(lastRemoteAckSeq, qp.seqs[j]) && remoteAcksIndex >= 0 && remoteAcksIndex < 32)
            {
                for (const auto &opkt : recentOutgoingPackets)
                {
                    if (opkt.seq == qp.seqs[j] && opkt.ackTime > 0)
                    {
                        LOGD("did ack seq %u, removing", qp.seqs[j]);
                        didAck = true;
                        break;
                    }
                }
                if (didAck)
                    break;
            }
        }
        if (didAck)
        {
            it = queuedPackets.erase(it);
            continue;
        }
        ++it;
    }
}