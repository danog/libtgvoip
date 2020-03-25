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
    //size_t len = npacket.data->Length();
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
        ProcessExtraData(extra, srcEndpoint);
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

        //LOGE("RECV: For pts %u = seq %u, got seq %u", pts, pts/60 + 1, pseq);

        //LOGD("stream data, pts=%d, len=%d, rem=%d", pts, sdlen, in.Remaining());
        auto *_stm = GetStreamByID<IncomingStream>(packet.streamId);
        if (!_stm)
        {
            LOGW("received packet for unknown stream %u", (unsigned int)packet.streamId);
            return;
        }
        if (!audioOutStarted && audioOutput)
        {
            MutexGuard m(audioIOMutex);
            audioOutput->Start();
            audioOutStarted = true;
        }
        if (_stm->type == StreamType::Audio)
        {
            auto *stm = dynamic_cast<IncomingAudioStream *>(_stm);
            uint32_t pts = packet.seq * stm->frameDuration;
            if (stm->jitterBuffer)
            {
                stm->jitterBuffer->HandleInput(std::move(packet.data), pts, false);
                /*if (peerVersion >= PROTOCOL_RELIABLE)
                    {
                        // Technically I should be using the specific packet manager's rtt history but will separate later
                        uint32_t tooOldSeq = stm->jitterBuffer->GetSeqTooLate(rttHistory[0]) - 1;
                        LOGW("Reverse acking seqs older than %u, newest acked seq %u (transportId %hhu)", tooOldSeq, manager.getLastRemoteSeq(), transportId);
                        manager.ackRemoteSeqsOlderThan(tooOldSeq);
                    }*/
                if (packet.extraEC)
                {
                    for (uint8_t i = 0; i < 8; i++)
                    {
                        if (packet.extraEC.v[i])
                        {
                            stm->jitterBuffer->HandleInput(std::move(packet.extraEC.v[i].get<Bytes>().data), pts - (8 - i) * stm->frameDuration, true);
                        }
                    }
                }
            }
        }
        else if (_stm->type == StreamType::Video)
        {
            auto *stm = dynamic_cast<IncomingVideoStream *>(_stm);
            if (stm->packetReassembler)
            { /*
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
                stm->packetReassembler->AddFragment(std::move(pdata), fragmentIndex, fragmentCount, pts, frameSeq, keyframe, rotation);*/
            }
            //LOGV("Received video fragment %u of %u", fragmentIndex, fragmentCount);
        }
    }
}

void VoIPController::ProcessExtraData(const Wrapped<Extra> &_data, Endpoint &srcEndpoint)
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
            for (const auto &decoder : data.decoders)
            {
                {
                    peerVideoDecoders.push_back(decoder);
                    LOGD("%c%c%c%c", PRINT_FOURCC(decoder.data))
                }
                ver.maxVideoResolution = data.maxResolution;

                SetupOutgoingVideoStream();
            }

            auto ack = std::make_shared<ExtraInitAck>();
            ack->peerVersion = PROTOCOL_VERSION;
            ack->minVersion = MIN_PROTOCOL_VERSION;

            for (const auto &stream : outgoingStreams)
            {
                ack->streams.v.push_back(*dynamic_pointer_cast<MediaStreamInfo>(stream));
            }

            LOGI("Sending init ack");
            SendExtra(ack);
            SendNopPacket();

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
    }
    else if (type == ExtraInitAck::ID)
    {
        auto &data = _data.get<ExtraInitAck>();
        LOGD("Received init ack");

        if (!receivedInitAck)
        {
            receivedInitAck = true;

            messageThread.Cancel(initTimeoutID);
            initTimeoutID = MessageThread::INVALID_ID;

            ver.peerVersion = data.peerVersion;
            if (data.minVersion > PROTOCOL_VERSION || data.peerVersion < MIN_PROTOCOL_VERSION)
            {
                lastError = ERROR_INCOMPATIBLE;

                SetState(STATE_FAILED);
                return;
            }

            LOGI("peer version from init ack %d", ver.peerVersion);

            shared_ptr<IncomingAudioStream> incomingAudioStream;
            for (const auto &stmInfo : data.streams)
            {
                std::shared_ptr<IncomingStream> stm;
                if (stmInfo.type == StreamType::Video)
                {
                    if (ver.peerVersion < 9)
                    {
                        LOGV("Skipping video stream for old protocol version");
                        continue;
                    }
                    auto vStm = std::make_shared<IncomingVideoStream>();
                    vStm->codec = stmInfo.codec;
                    vStm->packetReassembler = std::make_shared<PacketReassembler>();
                    vStm->packetReassembler->SetCallback(bind(&VoIPController::ProcessIncomingVideoFrame, this, placeholders::_1, placeholders::_2, placeholders::_3, placeholders::_4));

                    stm = dynamic_pointer_cast<IncomingStream>(vStm);
                }
                else if (stmInfo.type == StreamType::Audio)
                {
                    auto aStm = std::make_shared<IncomingAudioStream>();
                    aStm->codec = stmInfo.codec;
                    aStm->frameDuration = 60;

                    aStm->jitterBuffer = make_shared<JitterBuffer>(aStm->frameDuration);
                    if (aStm->frameDuration > 50)
                        aStm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_60", 2));
                    else if (aStm->frameDuration > 30)
                        aStm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_40", 4));
                    else
                        aStm->jitterBuffer->SetMinPacketCount(ServerConfig::GetSharedInstance()->GetUInt("jitter_initial_delay_20", 6));

                    if (!incomingAudioStream)
                        incomingAudioStream = aStm;

                    stm = dynamic_pointer_cast<IncomingStream>(aStm);
                }
                else if (stmInfo.type == StreamType::Signaling)
                {
                    stm = std::make_shared<IncomingStream>(stmInfo.streamId, stmInfo.type);
                }
                else
                {
                    LOGW("Unknown incoming stream type: %hhu", stmInfo.type);
                    continue;
                }
                stm->id = stmInfo.streamId;
                stm->type = stmInfo.type;
                stm->enabled = stmInfo.enabled;
                incomingStreams.push_back(std::move(stm));
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
    else if (type == ExtraStreamFlags::ID)
    {
        auto &data = _data.get<ExtraStreamFlags>();
        LOGV("Peer stream state: id %u flags %u", (unsigned int)data.streamId, (unsigned int)data.flags);
        for (auto &s : incomingStreams)
        {
            if (s->id == data.streamId)
            {
                bool prevEnabled = s->enabled;
                bool prevPaused = s->paused;
                s->enabled = data.flags & ExtraStreamFlags::Flags::Enabled;
                s->paused = data.flags & ExtraStreamFlags::Flags::Paused;
                if (s->type == StreamType::Audio)
                {
                    auto astm = dynamic_pointer_cast<IncomingAudioStream>(s);
                    if (data.flags & ExtraStreamFlags::Flags::ExtraEC)
                    {
                        if (!astm->extraECEnabled)
                        {
                            astm->extraECEnabled = true;
                            if (astm->jitterBuffer)
                                astm->jitterBuffer->SetMinPacketCount(4);
                        }
                    }
                    else
                    {
                        if (astm->extraECEnabled)
                        {
                            astm->extraECEnabled = false;
                            if (astm->jitterBuffer)
                                astm->jitterBuffer->SetMinPacketCount(2);
                        }
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
        auto &data = _data.get<ExtraStreamCsd>();
        LOGI("Received codec specific data");
        auto *stm = GetStreamByID<IncomingVideoStream>(data.streamId);
        if (stm)
        {
            stm->codecSpecificData.clear();
            stm->csdIsValid = false;
            stm->width = data.width;
            stm->height = data.height;
            for (auto &v : data.data)
            {
                stm->codecSpecificData.push_back(std::move(*v.data));
            }
        }
    }
    else if (type == ExtraLanEndpoint::ID)
    {
        if (!allowP2p)
            return;
        auto &data = _data.get<ExtraLanEndpoint>();
        LOGV("received lan endpoint (extra)");
        if (currentEndpoint == Endpoint::ID::LANv4)
            currentEndpoint = preferredRelay;

        unsigned char peerTag[16];
        Endpoint lan(Endpoint::ID::LANv4, data.port, data.address, NetworkAddress::Empty(), Endpoint::Type::UDP_P2P_LAN, peerTag);
        MutexGuard m(endpointsMutex);
        endpoints[Endpoint::ID::LANv4] = lan;
    }
    else if (type == ExtraNetworkChanged::ID)
    {
        LOGI("Peer network changed");
        auto &data = _data.get<ExtraNetworkChanged>();
        wasNetworkHandover = true;
        const Endpoint &_currentEndpoint = endpoints.at(currentEndpoint);
        if (_currentEndpoint.type != Endpoint::Type::UDP_RELAY && _currentEndpoint.type != Endpoint::Type::TCP_RELAY)
            currentEndpoint = preferredRelay;
        if (allowP2p)
            SendPublicEndpointsRequest();

        if (ver.peerVersion < 2)
        {
            return;
        }
        dataSavingRequestedByPeer = data.flags & ExtraInit::Flags::DataSavingEnabled;
        UpdateDataSavingState();
        UpdateAudioBitrateLimit();
        ResetEndpointPingStats();
    }
    else if (type == ExtraGroupCallKey::ID)
    {
        if (!didReceiveGroupCallKey && !didSendGroupCallKey)
        {
            auto &data = _data.get<ExtraGroupCallKey>();
            messageThread.Post([this, &data] {
                if (callbacks.groupCallKeyReceived)
                    callbacks.groupCallKeyReceived(this, *data.key);
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

        auto &data = _data.get<ExtraIpv6Endpoint>();
        peerIPv6Available = true;
        LOGV("Received peer IPv6 endpoint [%s]:%u", data.address.ToString().c_str(), data.port);

        Endpoint ep;
        ep.type = Endpoint::Type::UDP_P2P_INET;
        ep.port = data.port;
        ep.v6address = data.address;
        ep.id = Endpoint::ID::P2Pv6;
        endpoints[Endpoint::ID::P2Pv6] = ep;
        if (!myIPv6.IsEmpty())
            currentEndpoint = Endpoint::ID::P2Pv6;
    }
    else if (type == ExtraPing::ID)
    {
        //LOGD("Received ping from %s:%d", srcEndpoint.address.ToString().c_str(), srcEndpoint.port);
        if (srcEndpoint.type != Endpoint::Type::UDP_RELAY && srcEndpoint.type != Endpoint::Type::TCP_RELAY && !allowP2p)
        {
            LOGW("Received p2p ping but p2p is disabled by manual override");
            return;
        }
        auto pong = std::make_shared<ExtraPong>();
        SendExtra(pong);
        SendNopPacket();
    }
    else if (type == ExtraPong::ID)
    {
        auto &data = _data.get<ExtraPong>();
        if (data.seq)
        {
#ifdef LOG_PACKETS
            LOGD("Received pong for ping in seq %u", data.seq);
#endif
            if (data.seq == srcEndpoint.lastPingSeq)
            {
                srcEndpoint.rtts.Add(GetCurrentTime() - srcEndpoint.lastPingTime);
                srcEndpoint.averageRTT = srcEndpoint.rtts.NonZeroAverage();
                LOGD("Current RTT via %s: %.3f, average: %.3f", srcEndpoint.address.ToString().c_str(), srcEndpoint.rtts[0], srcEndpoint.averageRTT);
                if (srcEndpoint.averageRTT > rateMaxAcceptableRTT)
                    needRate = true;
            }
        }
    }
    /*
    else if (type == PKT_STREAM_EC)
    {
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
    }*/
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
