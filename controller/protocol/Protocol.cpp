#include "../PrivateDefines.cpp"
#include "packets/PacketManager.h"

using namespace tgvoip;
using namespace std;

#pragma mark - Networking & crypto

PacketManager &VoIPController::getBestPacketManager()
{
    return outgoingStreams[ver.isNew() ? StreamId::Audio : StreamId::Signaling]->packetManager;
}

void VoIPController::ProcessIncomingPacket(NetworkPacket &npacket, Endpoint &srcEndpoint)
{
    ENFORCE_MSG_THREAD;

    // Initial packet decryption and recognition

    unsigned char *buffer = **npacket.data;
    size_t len = npacket.data->Length();
    BufferInputStream in(*npacket.data);
    if (ver.peerVersion < 9 || srcEndpoint.IsReflector())
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
        if (srcEndpoint.port != npacket.port || srcEndpoint.address != npacket.address)
        {
            if (!npacket.address.isIPv6)
            {
                LOGI("Incoming packet was decrypted successfully, changing P2P endpoint to %s:%u", npacket.address.ToString().c_str(), npacket.port);
                srcEndpoint.address = npacket.address;
                srcEndpoint.port = npacket.port;
            }
        }
    }

    Packet packet;
    if (!packet.parse(in, ver))
    {
        LOGW("Failure parsing incoming packet!");
    }
    packetsReceived++;
    ProcessIncomingPacket(packet, srcEndpoint);

    if (packet.otherPackets.size())
    { // Legacy for PKT_STREAM_X2-3
        for (Packet &packet : packet.otherPackets)
        {
            ProcessIncomingPacket(packet, srcEndpoint);
        }
    }
}

void VoIPController::ProcessIncomingPacket(Packet &packet, Endpoint &srcEndpoint)
{
    // Use packet manager of outgoing stream
    PacketManager &manager = outgoingStreams[packet.legacy ? StreamId::Signaling : packet.streamId]->packetManager;

    if (!manager.ackRemoteSeq(packet))
    {
        return;
    }

#ifdef LOG_PACKETSackId
    LOGV("Received: from=%s:%u, seq=%u, length=%u, type=%s, transportId=%hhu", srcEndpoint.GetAddress().ToString().c_str(), srcEndpoint.port, pseq, (unsigned int)packet.data.Length(), GetPacketTypeString(type).c_str(), transportId);
#endif

    for (auto &extra : packet.extraSignaling)
    {
        ProcessExtraData(extra);
    }

    auto &sender = outgoingStreams[manager.transportId]->packetSender;
    if (packet.ackSeq && seqgt(packet.ackSeq, manager.getLastAckedSeq()))
    {
        if (waitingForAcks && manager.getLastAckedSeq() >= firstSentPing)
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
        conctl.PacketAcknowledged(CongestionControlPacket(packet));
        manager.ackLocal(packet.ackSeq, packet.ackMask);

        for (auto &opkt : manager.getRecentOutgoingPackets())
        {
            if (!opkt.ackTime && manager.wasLocalAcked(opkt.pkt.seq))
            {
                opkt.ackTime = GetCurrentTime();
                opkt.rttTime = opkt.ackTime - opkt.sendTime;
                if (opkt.lost)
                {
                    LOGW("acknowledged lost packet %u (streamId %hhu)", opkt.pkt.seq, packet.streamId);
                    sendLosses--;
                }
                else // Don't report lost packets as acked?
                {
                    sender->PacketAcknowledged(opkt);
                }

                conctl.PacketAcknowledged(opkt.pkt);
            }
        }

        HandleReliablePackets(manager);
    }

    Endpoint &_currentEndpoint = endpoints.at(currentEndpoint);
    if (srcEndpoint.id != currentEndpoint && srcEndpoint.IsReflector() && (_currentEndpoint.IsP2P() || _currentEndpoint.averageRTT == 0))
    {
        if (seqgt(manager.getLastSentSeq() - 32, manager.getLastAckedSeq()))
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
        SendNopPacket();
    }

    //LOGV("acks: %u -> %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf", manager.getLastAckedSeq()(), remoteAcks[0], remoteAcks[1], remoteAcks[2], remoteAcks[3], remoteAcks[4], remoteAcks[5], remoteAcks[6], remoteAcks[7]);
    //LOGD("recv: %u -> %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf, %.2lf", getLastRemoteSeq(), recvPacketTimes[0], recvPacketTimes[1], recvPacketTimes[2], recvPacketTimes[3], recvPacketTimes[4], recvPacketTimes[5], recvPacketTimes[6], recvPacketTimes[7]);
    //LOGI("RTT = %.3lf", GetAverageRTT());
    //LOGV("Packet %u type is %d", pseq, type);
    if (packet.data)
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
            if (stm && stm->type == StreamType::Audio)
            {
                if (stm->jitterBuffer)
                {
                    stm->jitterBuffer->HandleInput(static_cast<unsigned char *>(buffer + in.GetOffset()), sdlen, pts, false);
                    /*if (peerVersion >= PROTOCOL_RELIABLE)
                    {
                        // Technically I should be using the specific packet manager's rtt history but will separate later
                        uint32_t tooOldSeq = stm->jitterBuffer->GetSeqTooLate(rttHistory[0]) - 1;
                        LOGW("Reverse acking seqs older than %u, newest acked seq %u (transportId %hhu)", tooOldSeq, manager.getLastRemoteSeq(), transportId);
                        manager.ackRemoteSeqsOlderThan(tooOldSeq);
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
            else if (stm && stm->type == StreamType::Video)
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
        unsigned char peerTag[16];
        Endpoint lan(Endpoint::ID::LANv4, peerPort, NetworkAddress::IPv4(peerAddr), NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_LAN, peerTag);

        if (currentEndpoint == Endpoint::ID::LANv4)
            currentEndpoint = preferredRelay;

        MutexGuard m(endpointsMutex);
        endpoints[Endpoint::ID::LANv4] = lan;
    }
    if (type == PKT_NETWORK_CHANGED && _currentEndpoint.IsP2P())
    {
        currentEndpoint = preferredRelay;
        if (allowP2p)
            SendPublicEndpointsRequest();
        if (peerVersion >= 2)
        {
            uint32_t flags = in.ReadUInt32();
            dataSavingRequestedByPeer = flags & ExtraInit::Flags::DataSavingEnabled;
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
            if (stm->type != StreamType::Video)
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

void VoIPController::ProcessExtraData(const Wrapped<Extra> &_data)
{
    auto type = _data.getID();
    if (lastReceivedExtrasByType[type] == _data.d->hash)
    {
        return;
    }
    lastReceivedExtrasByType[type] = _data.d->hash;
    LOGE("ProcessExtraData=%s", _data.print().c_str());

    if (type == ExtraInit::ID)
    {
        auto &data = _data.get<ExtraInit>();

        LOGD("Received init");
        if (!receivedInit)
            ver.peerVersion = data.peerVersion;
        LOGI("Peer version is %d", ver.peerVersion);

        if (data.minVersion > PROTOCOL_VERSION || data.minVersion < MIN_PROTOCOL_VERSION)
        {
            lastError = ERROR_INCOMPATIBLE;

            SetState(STATE_FAILED);
            return;
        }

        if (!receivedInit)
        {
            if (data.flags & ExtraInit::Flags::DataSavingEnabled)
            {
                dataSavingRequestedByPeer = true;
                UpdateDataSavingState();
                UpdateAudioBitrateLimit();
            }
            if (data.flags & ExtraInit::Flags::GroupCallSupported)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_GROUP_CALLS;
            }
            if (data.flags & ExtraInit::Flags::VideoRecvSupported)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_VIDEO_DISPLAY;
            }
            if (data.flags & ExtraInit::Flags::VideoSendSupported)
            {
                peerCapabilities |= TGVOIP_PEER_CAP_VIDEO_CAPTURE;
            }
        }

        if (!receivedInit && ((data.flags & ExtraInit::Flags::VideoSendSupported && config.enableVideoReceive) || (data.flags & ExtraInit::Flags::VideoRecvSupported && config.enableVideoSend)))
        {
            LOGD("Peer video decoders:");
            uint8_t numSupportedVideoDecoders = in.ReadByte();
            for (auto i = 0; i < numSupportedVideoDecoders; i++)
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
            if (ver.peerVersion < 5)
                out.WriteByte((unsigned char)(stream->codec == Codec::Opus ? CODEC_OPUS_OLD : 0));
            else
                out.WriteInt32(stream->codec);
            out.WriteInt16(stream->frameDuration);
            out.WriteByte((unsigned char)(stream->enabled ? 1 : 0));
        }
        LOGI("Sending init ack");
        size_t outLength = out.GetOffset();
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
                ver.peerVersion = in.ReadInt32();
                uint32_t minVer = in.ReadUInt32();
                if (minVer > PROTOCOL_VERSION || ver.peerVersion < MIN_PROTOCOL_VERSION)
                {
                    lastError = ERROR_INCOMPATIBLE;

                    SetState(STATE_FAILED);
                    return;
                }
            }
            else
            {
                ver.peerVersion = 1;
            }

            LOGI("peer version from init ack %d", ver.peerVersion);

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
                if (ver.peerVersion < 5)
                {
                    unsigned char codec = in.ReadByte();
                    if (codec == CODEC_OPUS_OLD)
                        stm->codec = Codec::Opus;
                }
                else
                {
                    stm->codec = in.ReadUInt32();
                }
                in.ReadInt16();
                stm->frameDuration = 60;
                stm->enabled = in.ReadByte() == 1;
                if (stm->type == StreamType::Video && ver.peerVersion < 9)
                {
                    LOGV("Skipping video stream for old protocol version");
                    continue;
                }
                if (stm->type == StreamType::Audio)
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
                else if (stm->type == StreamType::Video)
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
                if (stm->type == StreamType::Audio && !incomingAudioStream)
                    incomingAudioStream = stm;
            }
            if (!incomingAudioStream)
                return;

            if (ver.peerVersion >= 5 && !useMTProto2)
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
    if (type == ExtraStreamFlags::ID)
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
                s->enabled = flags & ExtraStreamFlags::Flags::Enabled;
                s->paused = flags & ExtraStreamFlags::Flags::Paused;
                if (flags & ExtraStreamFlags::Flags::ExtraEC)
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
                if (prevEnabled != s->enabled && s->type == StreamType::Video && videoRenderer)
                    videoRenderer->SetStreamEnabled(s->enabled);
                if (prevPaused != s->paused && s->type == StreamType::Video && videoRenderer)
                    videoRenderer->SetStreamPaused(s->paused);
                UpdateAudioOutputState();
                break;
            }
        }
    }
    else if (type == ExtraStreamCsd::ID)
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
    else if (type == ExtraLanEndpoint::ID)
    {
        if (!allowP2p)
            return;
        LOGV("received lan endpoint (extra)");
        uint32_t peerAddr = in.ReadUInt32();
        uint16_t peerPort = static_cast<uint16_t>(in.ReadInt32());
        if (currentEndpoint == Endpoint::ID::LANv4)
            currentEndpoint = preferredRelay;

        unsigned char peerTag[16];
        Endpoint lan(Endpoint::ID::LANv4, peerPort, NetworkAddress::IPv4(peerAddr), NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_LAN, peerTag);
        MutexGuard m(endpointsMutex);
        endpoints[Endpoint::ID::LANv4] = lan;
    }
    else if (type == ExtraNetworkChanged::ID)
    {
        LOGI("Peer network changed");
        wasNetworkHandover = true;
        const Endpoint &_currentEndpoint = endpoints.at(currentEndpoint);
        if (_currentEndpoint.type != Endpoint::Type::UDP_RELAY && _currentEndpoint.type != Endpoint::Type::TCP_RELAY)
            currentEndpoint = preferredRelay;
        if (allowP2p)
            SendPublicEndpointsRequest();
        uint32_t flags = in.ReadUInt32();
        dataSavingRequestedByPeer = flags & ExtraInit::Flags::DataSavingEnabled;
        UpdateDataSavingState();
        UpdateAudioBitrateLimit();
        ResetEndpointPingStats();
    }
    else if (type == ExtraGroupCallKey::ID)
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
    else if (type == ExtraGroupCallUpgrade::ID)
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
    else if (type == ExtraIpv6Endpoint::ID)
    {
        if (!allowP2p)
            return;
        NetworkAddress addr = NetworkAddress::IPv6(in);
        uint16_t port = in.ReadUInt16();
        peerIPv6Available = true;
        LOGV("Received peer IPv6 endpoint [%s]:%u", addr.ToString().c_str(), port);

        Endpoint ep;
        ep.type = Endpoint::Type::UDP_P2P_INET;
        ep.port = port;
        ep.v6address = addr;
        ep.id = Endpoint::ID::P2Pv6;
        endpoints[Endpoint::ID::P2Pv6] = ep;
        if (!myIPv6.IsEmpty())
            currentEndpoint = Endpoint::ID::P2Pv6;
    }
}

void VoIPController::ProcessAcknowledgedOutgoingExtra(UnacknowledgedExtraData &extra)
{
    if (extra.data.getID() == ExtraGroupCallKey::ID)
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