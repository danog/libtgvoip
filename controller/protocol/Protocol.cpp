#include "../PrivateDefines.cpp"
#include "PacketManager.h"

using namespace tgvoip;
using namespace std;

#pragma mark - Networking & crypto

PacketManager &VoIPController::getBestPacketManager()
{
#if PROTOCOL_VERSION == 9
    return packetManager;
#else
    return outgoingStreams.empty() ? packetManager : outgoingStreams[0]->packetSender->getPacketManager();
#endif
}

void VoIPController::ProcessIncomingPacket(NetworkPacket &packet, Endpoint &srcEndpoint)
{
    ENFORCE_MSG_THREAD;

    // Initial packet decryption and recognition

    unsigned char *buffer = *packet.data;
    size_t len = packet.data.Length();
    BufferInputStream in(packet.data);
    if (peerVersion < 9 || srcEndpoint.IsReflector())
    {
        if (in.Remaining() < 16)
        {
            LOGW("Received packet has wrong (no) peerTag");
            return;
        }
        if (memcmp(buffer, srcEndpoint.IsReflector() ? (void *)srcEndpoint.peerTag : (void *)callID, 16) != 0)
        {
            LOGW("Received packet has wrong peerTag");
            return;
        }
        in.Seek(16);
    }
    if (in.Remaining() >= 16 && srcEndpoint.IsReflector() && parseRelayPacket(in, srcEndpoint))
    {
        return;
    }
    if (in.Remaining() < 40)
    {
        LOGV("Received packet is too small");
        return;
    }

    size_t innerLen = decryptPacket(buffer, in);
    if (!innerLen) // Decryption failed
    {
        return;
    }

    lastRecvPacketTime = GetCurrentTime();

    if (state == STATE_RECONNECTING)
    {
        LOGI("Received a valid packet while reconnecting - setting state to established");
        SetState(STATE_ESTABLISHED);
    }

    if (srcEndpoint.type == Endpoint::Type::UDP_P2P_INET && !srcEndpoint.IsIPv6Only())
    {
        if (srcEndpoint.port != packet.port || srcEndpoint.address != packet.address)
        {
            if (!packet.address.isIPv6)
            {
                LOGI("Incoming packet was decrypted successfully, changing P2P endpoint to %s:%u", packet.address.ToString().c_str(), packet.port);
                srcEndpoint.address = packet.address;
                srcEndpoint.port = packet.port;
            }
        }
    }

    // decryptedAudioBlock random_id:long random_bytes:string flags:# voice_call_id:flags.2?int128 in_seq_no:flags.4?int out_seq_no:flags.4?int
    //   recent_received_mask:flags.5?int proto:flags.3?int extra:flags.1?string raw_data:flags.0?string = DecryptedAudioBlock
    //
    // simpleAudioBlock random_id:long random_bytes:string raw_data:string = DecryptedAudioBlock;

    // Version-specific extraction of packet fields ackId (last received packet seq on remote), (incoming packet seq) pseq, (ack mask) acks, (packet type) type, (flags) pflags, packet length
    uint32_t ackId;             // Last received packet seqno on remote
    uint32_t pseq;              // Incoming packet seqno
    uint32_t acks;              // Ack mask
    unsigned char type, pflags; // Packet type, flags
    uint8_t transportId = 0xFF; // Transport ID for reliable multiplexing
    size_t packetInnerLen = 0;
    if (peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92))
    {
        type = in.ReadByte();
        ackId = in.ReadUInt32();
        pseq = in.ReadUInt32();
        acks = in.ReadUInt32();
        pflags = in.ReadByte();
        packetInnerLen = innerLen - 14;

        if (pflags & XPFLAG_HAS_TRANSPORT_ID)
        {
            transportId = in.ReadByte();
        }
    }
    else if (!legacyParsePacket(in, type, ackId, pseq, acks, pflags, packetInnerLen))
    {
        return;
    }
    packetsReceived++;

    // Use packet manager of outgoing streams on both sides (probably should separate in separate vector)
    PacketManager *manager = transportId == 0xFF ? &packetManager : &outgoingStreams[transportId]->packetSender->getPacketManager();

    if (!manager->ackRemoteSeq(pseq))
    {
        return;
    }

#ifdef LOG_PACKETS
    LOGV("Received: from=%s:%u, seq=%u, length=%u, type=%s, transportId=%hhu", srcEndpoint.GetAddress().ToString().c_str(), srcEndpoint.port, pseq, (unsigned int)packet.data.Length(), GetPacketTypeString(type).c_str(), transportId);
#endif

    // Extra data
    if (pflags & XPFLAG_HAS_EXTRA)
    {
        unsigned char extraCount = in.ReadByte();
        for (int i = 0; i < extraCount; i++)
        {
            size_t extraLen = in.ReadByte();
            Buffer xbuffer(extraLen);
            in.ReadBytes(*xbuffer, extraLen);
            ProcessExtraData(xbuffer);
        }
    }

    uint32_t recvTS = 0;
    if (pflags & XPFLAG_HAS_RECV_TS)
    {
        recvTS = in.ReadUInt32();
    }

    if (seqgt(ackId, manager->getLastAckedSeq()))
    {
        if (waitingForAcks && manager->getLastAckedSeq() >= firstSentPing)
        {
            rttHistory.Reset();
            waitingForAcks = false;
            dontSendPackets = 10;
            messageThread.Post(
                [this] {
                    dontSendPackets = 0;
                },
                1.0);
            LOGI("resuming sending");
        }
        conctl.PacketAcknowledged(ackId);

        manager->ackLocal(ackId, acks);

        if (transportId != 0xFF && !incomingStreams.empty() && incomingStreams[transportId]->jitterBuffer)
        {
            // Technically I should be using the specific packet manager's rtt history but will separate later
            uint32_t tooOldSeq = incomingStreams[transportId]->jitterBuffer->GetSeqTooLate(rttHistory[0]);
            LOGW("Reverse acking seqs older than %u, newest seq received from remote %u (transportId %hhu)", tooOldSeq, manager->getLastRemoteSeq(), transportId);
            manager->ackRemoteSeqsOlderThan(tooOldSeq);
        }

        for (auto &opkt : manager->getRecentOutgoingPackets())
        {
            if (opkt.ackTime)
            {
                continue;
            }
            if (manager->wasLocalAcked(opkt.seq))
            {
                opkt.data = Buffer();
                opkt.ackTime = GetCurrentTime();
                opkt.rttTime = opkt.ackTime - opkt.sendTime;
                if (opkt.lost)
                {
                    LOGW("acknowledged lost packet %u (transportId %hhu)", opkt.seq, transportId);
                    sendLosses--;
                }
                if (opkt.sender && !opkt.lost)
                { // don't report lost packets as acknowledged to PacketSenders
                    opkt.sender->PacketAcknowledged(opkt.seq, opkt.sendTime, recvTS / 1000.0f, opkt.type, opkt.size);
                }

                // TODO move this to a PacketSender
                conctl.PacketAcknowledged(opkt.seq);
            }
            else if (peerVersion >= PROTOCOL_RELIABLE && opkt.data.Length() && opkt.seq < manager->getLastAckedSeq())
            {
                if (manager->getLastAckedSeq() - opkt.seq > 32)
                {
                    LOGW("Marking reliable NACK packet %u as lost, since is more than 32 seqs old (last acked %u, transportId %hhu)", opkt.seq, manager->getLastAckedSeq(), transportId);
                    opkt.lost = true;
                    opkt.data = Buffer();
                    continue;
                }
                if (opkt.sendTime + rttHistory[0] < VoIPController::GetCurrentTime())
                {
                    LOGW("It is possibly a bit too early to resend NACK packet %u (transportId %hhu)", opkt.seq, transportId);
                }
                LOGW("Resending reliable NACK packet %u, (transportId %hhu)", opkt.seq, transportId);
                BufferOutputStream copy(opkt.data.Length());
                copy.WriteBytes(opkt.data);
                Endpoint *endpoint = GetEndpointById(opkt.endpoint);
                if (!endpoint)
                {
                    LOGE("Recent NACK queue contained packet (%u) for nonexistent endpoint, (transportId %hhu)", opkt.seq, transportId);
                    opkt.lost = true;
                    opkt.data = Buffer();
                    continue;
                }
                opkt.sendTime = GetCurrentTime();
                SendPacket(copy.GetBuffer(), copy.GetLength(), *endpoint, opkt.seq, opkt.type, transportId);
                //SendOrEnqueuePacket()
            }
        }

        if (peerVersion >= 6)
        {
            for (auto x = currentExtras.begin(); x != currentExtras.end();)
            {
                if (x->firstContainingSeq != 0 && seqgte(manager->getLastAckedSeq(), x->firstContainingSeq))
                {
                    LOGV("Peer acknowledged extra type %u length %u", x->type, (unsigned int)x->data.Length());
                    ProcessAcknowledgedOutgoingExtra(*x);
                    x = currentExtras.erase(x);
                    continue;
                }
                ++x;
            }
        }
        if (peerVersion < PROTOCOL_RELIABLE)
            handleReliablePackets(); // Use old reliability logic
    }

    Endpoint &_currentEndpoint = endpoints.at(currentEndpoint);
    if (srcEndpoint.id != currentEndpoint && srcEndpoint.IsReflector() && (_currentEndpoint.IsP2P() || _currentEndpoint.averageRTT == 0))
    {
        if (seqgt(manager->getLastSentSeq() - 32, manager->getLastAckedSeq()))
        {
            currentEndpoint = srcEndpoint.id;
            _currentEndpoint = srcEndpoint;
            LOGI("Peer network address probably changed, switching to relay");
            if (allowP2p)
                SendPublicEndpointsRequest();
        }
    }

    /*
    if (config.logPacketStats)
    {
        DebugLoggedPacket dpkt = {
            static_cast<int32_t>(pseq),
            GetCurrentTime() - connectionInitTime,
            static_cast<int32_t>(packet.data.Length())};
        debugLoggedPackets.push_back(dpkt);
        if (debugLoggedPackets.size() >= 2500)
        {
            debugLoggedPackets.erase(debugLoggedPackets.begin(), debugLoggedPackets.begin() + 500);
        }
    }*/

    if (unacknowledgedIncomingPacketCount++ > unackNopThreshold)
    {
        //LOGV("Sending nop packet as ack");
        SendNopPacket(packetManager);
    }

    //LOGV("acks: %u -> %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf", manager.getLastAckedSeq()(), remoteAcks[0], remoteAcks[1], remoteAcks[2], remoteAcks[3], remoteAcks[4], remoteAcks[5], remoteAcks[6], remoteAcks[7]);
    //LOGD("recv: %u -> %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf", getLastRemoteSeq(), recvPacketTimes[0], recvPacketTimes[1], recvPacketTimes[2], recvPacketTimes[3], recvPacketTimes[4], recvPacketTimes[5], recvPacketTimes[6], recvPacketTimes[7]);
    //LOGI("RTT = %.3lf", GetAverageRTT());
    //LOGV("Packet %u type is %d", pseq, type);
    if (type == PKT_INIT)
    {
        LOGD("Received init");
        uint32_t ver = in.ReadUInt32();
        if (!receivedInit)
            peerVersion = ver;
        LOGI("Peer version is %d", peerVersion);
        uint32_t minVer = in.ReadUInt32();
        if (minVer > PROTOCOL_VERSION || peerVersion < MIN_PROTOCOL_VERSION)
        {
            lastError = ERROR_INCOMPATIBLE;

            SetState(STATE_FAILED);
            return;
        }
        uint32_t flags = in.ReadUInt32();
        if (!receivedInit)
        {
            if (flags & INIT_FLAG_DATA_SAVING_ENABLED)
            {
                dataSavingRequestedByPeer = true;
                UpdateDataSavingState();
                UpdateAudioBitrateLimit();
            }
            if (flags & INIT_FLAG_GROUP_CALLS_SUPPORTED)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_GROUP_CALLS;
            }
            if (flags & INIT_FLAG_VIDEO_RECV_SUPPORTED)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_VIDEO_DISPLAY;
            }
            if (flags & INIT_FLAG_VIDEO_SEND_SUPPORTED)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_VIDEO_CAPTURE;
            }
        }

        unsigned int i;
        unsigned char numSupportedAudioCodecs = in.ReadByte();
        for (i = 0; i < numSupportedAudioCodecs; i++)
        {
            if (peerVersion < 5)
                in.ReadByte(); // ignore for now
            else
                in.ReadInt32();
        }
        if (!receivedInit && ((flags & INIT_FLAG_VIDEO_SEND_SUPPORTED && config.enableVideoReceive) || (flags & INIT_FLAG_VIDEO_RECV_SUPPORTED && config.enableVideoSend)))
        {
            LOGD("Peer video decoders:");
            unsigned int numSupportedVideoDecoders = in.ReadByte();
            for (i = 0; i < numSupportedVideoDecoders; i++)
            {
                uint32_t id = in.ReadUInt32();
                peerVideoDecoders.push_back(id);
                char *_id = reinterpret_cast<char *>(&id);
                LOGD("%c%c%c%c", _id[3], _id[2], _id[1], _id[0]);
            }
            protocolInfo.maxVideoResolution = in.ReadByte();

            SetupOutgoingVideoStream();
        }

        BufferOutputStream out(1024);

        out.WriteInt32(PROTOCOL_VERSION);
        out.WriteInt32(MIN_PROTOCOL_VERSION);

        out.WriteByte((unsigned char)outgoingStreams.size());
        for (const auto &stream : outgoingStreams)
        {
            out.WriteByte(stream->id);
            out.WriteByte(stream->type);
            if (peerVersion < 5)
                out.WriteByte((unsigned char)(stream->codec == CODEC_OPUS ? CODEC_OPUS_OLD : 0));
            else
                out.WriteInt32(stream->codec);
            out.WriteInt16(stream->frameDuration);
            out.WriteByte((unsigned char)(stream->enabled ? 1 : 0));
        }
        LOGI("Sending init ack");
        size_t outLength = out.GetLength();
        SendOrEnqueuePacket(PendingOutgoingPacket{
            /*.seq=*/packetManager.nextLocalSeq(),
            /*.type=*/PKT_INIT_ACK,
            /*.len=*/outLength,
            /*.data=*/Buffer(move(out)),
            /*.endpoint=*/0});
        if (!receivedInit)
        {
            receivedInit = true;
            if ((srcEndpoint.type == Endpoint::Type::UDP_RELAY && udpConnectivityState != UDP_BAD && udpConnectivityState != UDP_NOT_AVAILABLE) || srcEndpoint.type == Endpoint::Type::TCP_RELAY)
            {
                currentEndpoint = srcEndpoint.id;
                if (srcEndpoint.type == Endpoint::Type::UDP_RELAY || (useTCP && srcEndpoint.type == Endpoint::Type::TCP_RELAY))
                    preferredRelay = srcEndpoint.id;
            }
        }
        if (!audioStarted && receivedInitAck)
        {
            StartAudio();
            audioStarted = true;
        }
    }
    if (type == PKT_INIT_ACK)
    {
        LOGD("Received init ack");

        if (!receivedInitAck)
        {
            receivedInitAck = true;

            messageThread.Cancel(initTimeoutID);
            initTimeoutID = MessageThread::INVALID_ID;

            if (packetInnerLen > 10)
            {
                peerVersion = in.ReadInt32();
                uint32_t minVer = in.ReadUInt32();
                if (minVer > PROTOCOL_VERSION || peerVersion < MIN_PROTOCOL_VERSION)
                {
                    lastError = ERROR_INCOMPATIBLE;

                    SetState(STATE_FAILED);
                    return;
                }
            }
            else
            {
                peerVersion = 1;
            }

            LOGI("peer version from init ack %d", peerVersion);

            unsigned char streamCount = in.ReadByte();
            if (streamCount == 0)
                return;

            int i;
            shared_ptr<Stream> incomingAudioStream = nullptr;
            for (i = 0; i < streamCount; i++)
            {
                shared_ptr<Stream> stm = make_shared<Stream>();
                stm->id = in.ReadByte();
                stm->type = static_cast<StreamType>(in.ReadByte());
                if (peerVersion < 5)
                {
                    unsigned char codec = in.ReadByte();
                    if (codec == CODEC_OPUS_OLD)
                        stm->codec = CODEC_OPUS;
                }
                else
                {
                    stm->codec = in.ReadUInt32();
                }
                in.ReadInt16();
                stm->frameDuration = 60;
                stm->enabled = in.ReadByte() == 1;
                if (stm->type == STREAM_TYPE_VIDEO && peerVersion < 9)
                {
                    LOGV("Skipping video stream for old protocol version");
                    continue;
                }
                if (stm->type == STREAM_TYPE_AUDIO)
                {
                    stm->jitterBuffer = make_shared<JitterBuffer>(stm->frameDuration);
                    if (stm->frameDuration > 50)
                        stm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_60", 2));
                    else if (stm->frameDuration > 30)
                        stm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_40", 4));
                    else
                        stm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_20", 6));
                    stm->decoder = nullptr;
                }
                else if (stm->type == STREAM_TYPE_VIDEO)
                {
                    if (!stm->packetReassembler)
                    {
                        stm->packetReassembler = make_shared<PacketReassembler>();
                        stm->packetReassembler->SetCallback(bind(&VoIPController::ProcessIncomingVideoFrame, this, placeholders::_1, placeholders::_2, placeholders::_3, placeholders::_4));
                    }
                }
                else
                {
                    LOGW("Unknown incoming stream type: %d", stm->type);
                    continue;
                }
                incomingStreams.push_back(stm);
                if (stm->type == STREAM_TYPE_AUDIO && !incomingAudioStream)
                    incomingAudioStream = stm;
            }
            if (!incomingAudioStream)
                return;

            if (peerVersion >= 5 && !useMTProto2)
            {
                useMTProto2 = true;
                LOGD("MTProto2 wasn't initially enabled for whatever reason but peer supports it; upgrading");
            }

            if (!audioStarted && receivedInit)
            {
                StartAudio();
                audioStarted = true;
            }
            messageThread.Post(
                [this] {
                    if (state == STATE_WAIT_INIT_ACK)
                    {
                        SetState(STATE_ESTABLISHED);
                    }
                },
                ServerConfig::GetSharedInstance()->GetDouble("established_delay_if_no_stream_data", 1.5));
            if (allowP2p)
                SendPublicEndpointsRequest();
        }
    }
    if (type == PKT_STREAM_DATA || type == PKT_STREAM_DATA_X2 || type == PKT_STREAM_DATA_X3)
    {
        if (!receivedFirstStreamPacket)
        {
            receivedFirstStreamPacket = true;
            if (state != STATE_ESTABLISHED && receivedInitAck)
            {
                messageThread.Post(
                    [this]() {
                        SetState(STATE_ESTABLISHED);
                    },
                    .5);
                LOGW("First audio packet - setting state to ESTABLISHED");
            }
        }
        if (srcEndpoint.type == Endpoint::Type::UDP_RELAY && srcEndpoint.id != peerPreferredRelay)
        {
            peerPreferredRelay = srcEndpoint.id;
        }

        uint8_t count = 1;
        switch (type)
        {
        case PKT_STREAM_DATA_X2:
            count = 2;
            break;
        case PKT_STREAM_DATA_X3:
            count = 3;
            break;
        }
        uint8_t i;
        for (i = 0; i < count; i++)
        {
            unsigned char streamID = in.ReadByte();
            unsigned char flags = (unsigned char)(streamID & 0xC0);
            streamID &= 0x3F;
            uint16_t sdlen = (uint16_t)(flags & STREAM_DATA_FLAG_LEN16 ? in.ReadInt16() : in.ReadByte());
            uint32_t pts = in.ReadUInt32();
            unsigned char fragmentCount = 1;
            unsigned char fragmentIndex = 0;
            //LOGE("RECV: For pts %u = seq %u, got seq %u", pts, pts/60 + 1, pseq);

            //LOGD("stream data, pts=%d, len=%d, rem=%d", pts, sdlen, in.Remaining());
            audioTimestampIn = pts;
            if (!audioOutStarted && audioOutput)
            {
                MutexGuard m(audioIOMutex);
                audioOutput->Start();
                audioOutStarted = true;
            }
            bool fragmented = static_cast<bool>(sdlen & STREAM_DATA_XFLAG_FRAGMENTED);
            bool extraFEC = static_cast<bool>(sdlen & STREAM_DATA_XFLAG_EXTRA_FEC);
            bool keyframe = static_cast<bool>(sdlen & STREAM_DATA_XFLAG_KEYFRAME);
            if (fragmented)
            {
                fragmentIndex = in.ReadByte();
                fragmentCount = in.ReadByte();
            }
            sdlen &= 0x7FF;
            if (in.GetOffset() + sdlen > len)
            {
                return;
            }
            shared_ptr<Stream> stm;
            for (shared_ptr<Stream> &ss : incomingStreams)
            {
                if (ss->id == streamID)
                {
                    stm = ss;
                    break;
                }
            }
            if (stm && stm->type == STREAM_TYPE_AUDIO)
            {
                if (stm->jitterBuffer)
                {
                    stm->jitterBuffer->HandleInput(static_cast<unsigned char *>(buffer + in.GetOffset()), sdlen, pts, false);
                    /*if (peerVersion >= PROTOCOL_RELIABLE)
                    {
                        // Technically I should be using the specific packet manager's rtt history but will separate later
                        uint32_t tooOldSeq = stm->jitterBuffer->GetSeqTooLate(rttHistory[0]) - 1;
                        LOGW("Reverse acking seqs older than %u, newest acked seq %u (transportId %hhu)", tooOldSeq, manager->getLastRemoteSeq(), transportId);
                        manager->ackRemoteSeqsOlderThan(tooOldSeq);
                    }*/
                    if (extraFEC)
                    {
                        in.Seek(in.GetOffset() + sdlen);
                        unsigned int fecCount = in.ReadByte();
                        for (unsigned int j = 0; j < fecCount; j++)
                        {
                            unsigned char dlen = in.ReadByte();
                            unsigned char data[256];
                            in.ReadBytes(data, dlen);
                            stm->jitterBuffer->HandleInput(data, dlen, pts - (fecCount - j) * stm->frameDuration, true);
                        }
                    }
                }
            }
            else if (stm && stm->type == STREAM_TYPE_VIDEO)
            {
                if (stm->packetReassembler)
                {
                    uint8_t frameSeq = in.ReadByte();
                    Buffer pdata(sdlen);
                    uint16_t rotation = 0;
                    if (fragmentIndex == 0)
                    {
                        unsigned char _rotation = in.ReadByte() & (unsigned char)VIDEO_ROTATION_MASK;
                        switch (_rotation)
                        {
                        case VIDEO_ROTATION_0:
                            rotation = 0;
                            break;
                        case VIDEO_ROTATION_90:
                            rotation = 90;
                            break;
                        case VIDEO_ROTATION_180:
                            rotation = 180;
                            break;
                        case VIDEO_ROTATION_270:
                            rotation = 270;
                            break;
                        default: // unreachable on sane CPUs
                            abort();
                        }
                        //if(rotation!=stm->rotation){
                        //	stm->rotation=rotation;
                        //	LOGI("Video rotation: %u", rotation);
                        //}
                    }
                    pdata.CopyFrom(buffer + in.GetOffset(), 0, sdlen);
                    stm->packetReassembler->AddFragment(std::move(pdata), fragmentIndex, fragmentCount, pts, frameSeq, keyframe, rotation);
                }
                //LOGV("Received video fragment %u of %u", fragmentIndex, fragmentCount);
            }
            else
            {
                LOGW("received packet for unknown stream %u", (unsigned int)streamID);
            }
            if (i < count - 1)
                in.Seek(in.GetOffset() + sdlen);
        }
    }
    if (type == PKT_PING)
    {
        //LOGD("Received ping from %s:%d", srcEndpoint.address.ToString().c_str(), srcEndpoint.port);
        if (srcEndpoint.type != Endpoint::Type::UDP_RELAY && srcEndpoint.type != Endpoint::Type::TCP_RELAY && !allowP2p)
        {
            LOGW("Received p2p ping but p2p is disabled by manual override");
            return;
        }
        BufferOutputStream pkt(128);
        pkt.WriteInt32(pseq);
        size_t pktLength = pkt.GetLength();
        SendOrEnqueuePacket(PendingOutgoingPacket{
            /*.seq=*/packetManager.nextLocalSeq(),
            /*.type=*/PKT_PONG,
            /*.len=*/pktLength,
            /*.data=*/Buffer(move(pkt)),
            /*.endpoint=*/srcEndpoint.id,
        });
    }
    if (type == PKT_PONG)
    {
        if (packetInnerLen >= 4)
        {
            uint32_t pingSeq = in.ReadUInt32();
#ifdef LOG_PACKETS
            LOGD("Received pong for ping in seq %u", pingSeq);
#endif
            if (pingSeq == srcEndpoint.lastPingSeq)
            {
                srcEndpoint.rtts.Add(GetCurrentTime() - srcEndpoint.lastPingTime);
                srcEndpoint.averageRTT = srcEndpoint.rtts.NonZeroAverage();
                LOGD("Current RTT via %s: %.3f, average: %.3f", packet.address.ToString().c_str(), srcEndpoint.rtts[0], srcEndpoint.averageRTT);
                if (srcEndpoint.averageRTT > rateMaxAcceptableRTT)
                    needRate = true;
            }
        }
    }
    if (type == PKT_STREAM_STATE)
    {
        unsigned char id = in.ReadByte();
        unsigned char enabled = in.ReadByte();
        LOGV("Peer stream state: id %u flags %u", (int)id, (int)enabled);
        for (vector<shared_ptr<Stream>>::iterator s = incomingStreams.begin(); s != incomingStreams.end(); ++s)
        {
            if ((*s)->id == id)
            {
                (*s)->enabled = enabled == 1;
                UpdateAudioOutputState();
                break;
            }
        }
    }
    if (type == PKT_LAN_ENDPOINT)
    {
        LOGV("received lan endpoint");
        uint32_t peerAddr = in.ReadUInt32();
        uint16_t peerPort = (uint16_t)in.ReadInt32();
        constexpr int64_t lanID = static_cast<int64_t>(FOURCC('L', 'A', 'N', '4')) << 32;
        unsigned char peerTag[16];
        Endpoint lan(lanID, peerPort, NetworkAddress::IPv4(peerAddr), NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_LAN, peerTag);

        if (currentEndpoint == lanID)
            currentEndpoint = preferredRelay;

        MutexGuard m(endpointsMutex);
        endpoints[lanID] = lan;
    }
    if (type == PKT_NETWORK_CHANGED && _currentEndpoint.IsP2P())
    {
        currentEndpoint = preferredRelay;
        if (allowP2p)
            SendPublicEndpointsRequest();
        if (peerVersion >= 2)
        {
            uint32_t flags = in.ReadUInt32();
            dataSavingRequestedByPeer = (flags & INIT_FLAG_DATA_SAVING_ENABLED) == INIT_FLAG_DATA_SAVING_ENABLED;
            UpdateDataSavingState();
            UpdateAudioBitrateLimit();
            ResetEndpointPingStats();
        }
    }
    if (type == PKT_STREAM_EC)
    {
        unsigned char streamID = in.ReadByte();
        if (peerVersion < 7)
        {
            uint32_t lastTimestamp = in.ReadUInt32();
            unsigned char count = in.ReadByte();
            for (shared_ptr<Stream> &stm : incomingStreams)
            {
                if (stm->id == streamID)
                {
                    for (unsigned int i = 0; i < count; i++)
                    {
                        unsigned char dlen = in.ReadByte();
                        unsigned char data[256];
                        in.ReadBytes(data, dlen);
                        if (stm->jitterBuffer)
                        {
                            stm->jitterBuffer->HandleInput(data, dlen, lastTimestamp - (count - i) * stm->frameDuration, true);
                        }
                    }
                    break;
                }
            }
        }
        else
        {
            shared_ptr<Stream> stm = GetStreamByID(streamID, false);
            if (!stm)
            {
                LOGW("Received FEC packet for unknown stream %u", streamID);
                return;
            }
            if (stm->type != STREAM_TYPE_VIDEO)
            {
                LOGW("Received FEC packet for non-video stream %u", streamID);
                return;
            }
            if (!stm->packetReassembler)
                return;

            uint8_t fseq = in.ReadByte();
            unsigned char fecScheme = in.ReadByte();
            unsigned char prevFrameCount = in.ReadByte();
            uint16_t fecLen = in.ReadUInt16();
            if (fecLen > in.Remaining())
                return;

            Buffer fecData(fecLen);
            in.ReadBytes(fecData);

            stm->packetReassembler->AddFEC(std::move(fecData), fseq, prevFrameCount, fecScheme);
        }
    }
}

void VoIPController::ProcessExtraData(Buffer &data)
{
    BufferInputStream in(*data, data.Length());
    unsigned char type = in.ReadByte();
    unsigned char fullHash[SHA1_LENGTH];
    crypto.sha1(*data, data.Length(), fullHash);
    uint64_t hash = *reinterpret_cast<uint64_t *>(fullHash);
    if (lastReceivedExtrasByType[type] == hash)
    {
        return;
    }
    LOGE("ProcessExtraData");
    lastReceivedExtrasByType[type] = hash;
    if (type == EXTRA_TYPE_STREAM_FLAGS)
    {
        unsigned char id = in.ReadByte();
        uint32_t flags = in.ReadUInt32();
        LOGV("Peer stream state: id %u flags %u", (unsigned int)id, (unsigned int)flags);
        for (shared_ptr<Stream> &s : incomingStreams)
        {
            if (s->id == id)
            {
                bool prevEnabled = s->enabled;
                bool prevPaused = s->paused;
                s->enabled = (flags & STREAM_FLAG_ENABLED) == STREAM_FLAG_ENABLED;
                s->paused = (flags & STREAM_FLAG_PAUSED) == STREAM_FLAG_PAUSED;
                if (flags & STREAM_FLAG_EXTRA_EC)
                {
                    if (!s->extraECEnabled)
                    {
                        s->extraECEnabled = true;
                        if (s->jitterBuffer)
                            s->jitterBuffer->SetMinPacketCount(4);
                    }
                }
                else
                {
                    if (s->extraECEnabled)
                    {
                        s->extraECEnabled = false;
                        if (s->jitterBuffer)
                            s->jitterBuffer->SetMinPacketCount(2);
                    }
                }
                if (prevEnabled != s->enabled && s->type == STREAM_TYPE_VIDEO && videoRenderer)
                    videoRenderer->SetStreamEnabled(s->enabled);
                if (prevPaused != s->paused && s->type == STREAM_TYPE_VIDEO && videoRenderer)
                    videoRenderer->SetStreamPaused(s->paused);
                UpdateAudioOutputState();
                break;
            }
        }
    }
    else if (type == EXTRA_TYPE_STREAM_CSD)
    {
        LOGI("Received codec specific data");
        unsigned char streamID = in.ReadByte();
        for (shared_ptr<Stream> &stm : incomingStreams)
        {
            if (stm->id == streamID)
            {
                stm->codecSpecificData.clear();
                stm->csdIsValid = false;
                stm->width = static_cast<unsigned int>(in.ReadInt16());
                stm->height = static_cast<unsigned int>(in.ReadInt16());
                uint8_t count = in.ReadByte();
                for (uint8_t i = 0; i < count; i++)
                {
                    size_t len = (size_t)in.ReadByte();
                    Buffer csd(len);
                    in.ReadBytes(*csd, len);
                    stm->codecSpecificData.push_back(move(csd));
                }
                break;
            }
        }
    }
    else if (type == EXTRA_TYPE_LAN_ENDPOINT)
    {
        if (!allowP2p)
            return;
        LOGV("received lan endpoint (extra)");
        uint32_t peerAddr = in.ReadUInt32();
        uint16_t peerPort = static_cast<uint16_t>(in.ReadInt32());
        constexpr int64_t lanID = static_cast<int64_t>(FOURCC('L', 'A', 'N', '4')) << 32;
        if (currentEndpoint == lanID)
            currentEndpoint = preferredRelay;

        unsigned char peerTag[16];
        Endpoint lan(lanID, peerPort, NetworkAddress::IPv4(peerAddr), NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_LAN, peerTag);
        MutexGuard m(endpointsMutex);
        endpoints[lanID] = lan;
    }
    else if (type == EXTRA_TYPE_NETWORK_CHANGED)
    {
        LOGI("Peer network changed");
        wasNetworkHandover = true;
        const Endpoint &_currentEndpoint = endpoints.at(currentEndpoint);
        if (_currentEndpoint.type != Endpoint::Type::UDP_RELAY && _currentEndpoint.type != Endpoint::Type::TCP_RELAY)
            currentEndpoint = preferredRelay;
        if (allowP2p)
            SendPublicEndpointsRequest();
        uint32_t flags = in.ReadUInt32();
        dataSavingRequestedByPeer = (flags & INIT_FLAG_DATA_SAVING_ENABLED) == INIT_FLAG_DATA_SAVING_ENABLED;
        UpdateDataSavingState();
        UpdateAudioBitrateLimit();
        ResetEndpointPingStats();
    }
    else if (type == EXTRA_TYPE_GROUP_CALL_KEY)
    {
        if (!didReceiveGroupCallKey && !didSendGroupCallKey)
        {
            unsigned char groupKey[256];
            in.ReadBytes(groupKey, 256);
            messageThread.Post([this, &groupKey] {
                if (callbacks.groupCallKeyReceived)
                    callbacks.groupCallKeyReceived(this, groupKey);
            });
            didReceiveGroupCallKey = true;
        }
    }
    else if (type == EXTRA_TYPE_REQUEST_GROUP)
    {
        if (!didInvokeUpgradeCallback)
        {
            messageThread.Post([this] {
                if (callbacks.upgradeToGroupCallRequested)
                    callbacks.upgradeToGroupCallRequested(this);
            });
            didInvokeUpgradeCallback = true;
        }
    }
    else if (type == EXTRA_TYPE_IPV6_ENDPOINT)
    {
        if (!allowP2p)
            return;
        NetworkAddress addr = NetworkAddress::IPv6(in);
        uint16_t port = in.ReadUInt16();
        peerIPv6Available = true;
        LOGV("Received peer IPv6 endpoint [%s]:%u", addr.ToString().c_str(), port);

        constexpr int64_t p2pID = static_cast<int64_t>(FOURCC('P', '2', 'P', '6')) << 32;

        Endpoint ep;
        ep.type = Endpoint::Type::UDP_P2P_INET;
        ep.port = port;
        ep.v6address = addr;
        ep.id = p2pID;
        endpoints[p2pID] = ep;
        if (!myIPv6.IsEmpty())
            currentEndpoint = p2pID;
    }
}

void VoIPController::ProcessAcknowledgedOutgoingExtra(UnacknowledgedExtraData &extra)
{
    if (extra.type == EXTRA_TYPE_GROUP_CALL_KEY)
    {
        if (!didReceiveGroupCallKeyAck)
        {
            didReceiveGroupCallKeyAck = true;
            messageThread.Post([this] {
                if (callbacks.groupCallKeySent)
                    callbacks.groupCallKeySent(this);
            });
        }
    }
}

uint8_t VoIPController::WritePacketHeader(PendingOutgoingPacket &pkt, BufferOutputStream &s, PacketSender *source)
{
    PacketManager &manager = peerVersion >= PROTOCOL_RELIABLE && source ? source->getPacketManager() : packetManager;
    uint32_t acks = manager.getRemoteAckMask();

    if (peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92))
    {
        s.WriteByte(pkt.type);
        s.WriteInt32(manager.getLastRemoteSeq());
        s.WriteInt32(pkt.seq);
        s.WriteInt32(acks);

        unsigned char flags = currentExtras.empty() ? 0 : XPFLAG_HAS_EXTRA;

        shared_ptr<Stream> videoStream = GetStreamByType(STREAM_TYPE_VIDEO, false);
        if (peerVersion >= 9 && videoStream && videoStream->enabled)
            flags |= XPFLAG_HAS_RECV_TS;

        if (peerVersion >= PROTOCOL_RELIABLE && manager.getTransportId() != 0xFF)
            flags |= XPFLAG_HAS_TRANSPORT_ID;

        s.WriteByte(flags);

        if (!currentExtras.empty())
        {
            s.WriteByte(static_cast<unsigned char>(currentExtras.size()));
            for (auto &x : currentExtras)
            {
                //LOGV("Writing extra into header: type %u, length %d", x.type, int(x.data.Length()));
                assert(x.data.Length() <= 254);
                s.WriteByte(static_cast<unsigned char>(x.data.Length() + 1));
                s.WriteByte(x.type);
                s.WriteBytes(*x.data, x.data.Length());
                if (x.firstContainingSeq == 0)
                    x.firstContainingSeq = pkt.seq;
            }
        }
        if (peerVersion >= 9 && videoStream && videoStream->enabled)
        {
            s.WriteUInt32((lastRecvPacketTime - connectionInitTime) * 1000.0);
        }
    }
    else
    {
        legacyWritePacketHeader(pkt.seq, acks, &s, pkt.type, pkt.len);
    }

    s.WriteBytes(pkt.data);

    Buffer copyBuf(s.GetLength());
    if (peerVersion >= PROTOCOL_RELIABLE)
        copyBuf.CopyFrom(s.GetBuffer(), 0, s.GetLength());

    unacknowledgedIncomingPacketCount = 0;

    auto &recentOutgoingPackets = manager.getRecentOutgoingPackets();

    recentOutgoingPackets.push_back(RecentOutgoingPacket{
        pkt.endpoint,
        std::move(copyBuf),
        pkt.seq,
        0,
        GetCurrentTime(),
        0,
        0,
        pkt.type,
        static_cast<uint32_t>(pkt.len),
        source,
        false});
    while (recentOutgoingPackets.size() > MAX_RECENT_PACKETS)
    {
        recentOutgoingPackets.erase(recentOutgoingPackets.begin());
    }
    manager.setLastSentSeq(pkt.seq);

    return manager.getTransportId();
    //LOGI("packet header size %d", s->GetLength());
}