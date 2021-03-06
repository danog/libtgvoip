//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#include "VoIPGroupController.h"
#include "controller/net/PacketSender.h"
#include "tools/logging.h"
#include "VoIPServerConfig.h"
#include "controller/PrivateDefines.h"
#include <assert.h>
#include <math.h>
#include <time.h>

using namespace tgvoip;
using namespace std;

VoIPGroupController::VoIPGroupController(int32_t timeDifference)
{
	userSelfID = 0;
	this->timeDifference = timeDifference;
	LOGV("Created VoIPGroupController; timeDifference=%d", timeDifference);
}

VoIPGroupController::~VoIPGroupController()
{
	if (audioOutput)
	{
		audioOutput->Stop();
	}
	LOGD("before stop audio mixer");
	audioMixer.Stop();
}

void VoIPGroupController::SetGroupCallInfo(unsigned char *encryptionKey, unsigned char *reflectorGroupTag, unsigned char *reflectorSelfTag, unsigned char *reflectorSelfSecret, unsigned char *reflectorSelfTagHash, int32_t selfUserID, NetworkAddress reflectorAddress, NetworkAddress reflectorAddressV6, uint16_t reflectorPort)
{
	Endpoint e;
	e.address = reflectorAddress;
	e.v6address = reflectorAddressV6;
	e.port = reflectorPort;
	memcpy(e.peerTag, reflectorGroupTag, 16);
	e.type = Endpoint::Type::UDP_RELAY;
	e.id = FOURCC('G', 'R', 'P', 'R');
	endpoints[e.id] = e;
	groupReflector = e;
	currentEndpoint = e.id;

	memcpy(this->encryptionKey, encryptionKey, 256);
	memcpy(this->reflectorSelfTag, reflectorSelfTag, 16);
	memcpy(this->reflectorSelfSecret, reflectorSelfSecret, 16);
	memcpy(this->reflectorSelfTagHash, reflectorSelfTagHash, 16);
	uint8_t sha256[SHA256_LENGTH];
	crypto.sha256((uint8_t *)encryptionKey, 256, sha256);
	memcpy(callID, sha256 + (SHA256_LENGTH - 16), 16);
	memcpy(keyFingerprint, sha256 + (SHA256_LENGTH - 16), 8);
	this->userSelfID = selfUserID;

	//LOGD("reflectorSelfTag = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", reflectorSelfTag[0], reflectorSelfTag[1], reflectorSelfTag[2], reflectorSelfTag[3], reflectorSelfTag[4], reflectorSelfTag[5], reflectorSelfTag[6], reflectorSelfTag[7], reflectorSelfTag[8], reflectorSelfTag[9], reflectorSelfTag[10], reflectorSelfTag[11], reflectorSelfTag[12], reflectorSelfTag[13], reflectorSelfTag[14], reflectorSelfTag[15]);
	//LOGD("reflectorSelfSecret = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", reflectorSelfSecret[0], reflectorSelfSecret[1], reflectorSelfSecret[2], reflectorSelfSecret[3], reflectorSelfSecret[4], reflectorSelfSecret[5], reflectorSelfSecret[6], reflectorSelfSecret[7], reflectorSelfSecret[8], reflectorSelfSecret[9], reflectorSelfSecret[10], reflectorSelfSecret[11], reflectorSelfSecret[12], reflectorSelfSecret[13], reflectorSelfSecret[14], reflectorSelfSecret[15]);
	//LOGD("reflectorSelfTagHash = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", reflectorSelfTagHash[0], reflectorSelfTagHash[1], reflectorSelfTagHash[2], reflectorSelfTagHash[3], reflectorSelfTagHash[4], reflectorSelfTagHash[5], reflectorSelfTagHash[6], reflectorSelfTagHash[7], reflectorSelfTagHash[8], reflectorSelfTagHash[9], reflectorSelfTagHash[10], reflectorSelfTagHash[11], reflectorSelfTagHash[12], reflectorSelfTagHash[13], reflectorSelfTagHash[14], reflectorSelfTagHash[15]);
}

void VoIPGroupController::AddGroupCallParticipant(int32_t userID, unsigned char *memberTagHash, unsigned char *serializedStreams, size_t streamsLength)
{
	if (userID == userSelfID)
		return;
	if (userSelfID == 0)
		return;
	//if(streamsLength==0)
	//	return;
	MutexGuard m(participantsMutex);
	LOGV("Adding group call user %d, streams length %u", userID, (unsigned int)streamsLength);

	for (vector<GroupCallParticipant>::iterator p = participants.begin(); p != participants.end(); ++p)
	{
		if (p->userID == userID)
		{
			LOGE("user %d already added", userID);
			abort();
			break;
		}
	}

	GroupCallParticipant p;
	p.userID = userID;
	memcpy(p.memberTagHash, memberTagHash, sizeof(p.memberTagHash));

	BufferInputStream ss(serializedStreams, streamsLength);
	vector<shared_ptr<Stream>> streams = DeserializeStreams(ss);

	unsigned char audioStreamID = 0;

	for (vector<shared_ptr<Stream>>::iterator _s = streams.begin(); _s != streams.end(); ++_s)
	{
		shared_ptr<Stream> &s = *_s;
		s->userID = userID;
		if (s->type == StreamInfo::Type::Audio && s->codec == Codec::Opus && !audioStreamID)
		{
			audioStreamID = s->id;
			s->jitterBuffer = make_shared<JitterBuffer>(s->frameDuration);
			if (s->frameDuration > 50)
				s->jitterBuffer->SetMinPacketCount((uint32_t)ServerConfig::GetSharedInstance()->GetInt("jitter_initial_delay_60", 2));
			else if (s->frameDuration > 30)
				s->jitterBuffer->SetMinPacketCount((uint32_t)ServerConfig::GetSharedInstance()->GetInt("jitter_initial_delay_40", 4));
			else
				s->jitterBuffer->SetMinPacketCount((uint32_t)ServerConfig::GetSharedInstance()->GetInt("jitter_initial_delay_20", 6));
			s->callbackWrapper = make_shared<CallbackWrapper>();
			s->decoder = make_shared<OpusDecoder>(s->callbackWrapper, false, false);
			s->decoder->SetJitterBuffer(s->jitterBuffer);
			s->decoder->SetFrameDuration(s->frameDuration);
			s->decoder->SetDTX(true);
			s->decoder->SetLevelMeter(p.levelMeter);
			audioMixer.AddInput(s->callbackWrapper);
		}
		incomingStreams.push_back(s);
	}

	if (!audioStreamID)
	{
		LOGW("User %d has no usable audio stream", userID);
	}

	p.streams.insert(p.streams.end(), streams.begin(), streams.end());
	participants.push_back(p);
	LOGI("Added group call participant %d", userID);
}

void VoIPGroupController::RemoveGroupCallParticipant(int32_t userID)
{
	MutexGuard m(participantsMutex);
	vector<shared_ptr<Stream>>::iterator stm = incomingStreams.begin();
	while (stm != incomingStreams.end())
	{
		if ((*stm)->userID == userID)
		{
			LOGI("Removed stream %d belonging to user %d", (*stm)->id, userID);
			audioMixer.RemoveInput((*stm)->callbackWrapper);
			(*stm)->decoder->Stop();
			stm = incomingStreams.erase(stm);
			continue;
		}
		++stm;
	}
	for (vector<GroupCallParticipant>::iterator p = participants.begin(); p != participants.end(); ++p)
	{
		if (p->userID == userID)
		{
			participants.erase(p);
			LOGI("Removed group call participant %d", userID);
			break;
		}
	}
}

vector<shared_ptr<VoIPController::Stream>> VoIPGroupController::DeserializeStreams(BufferInputStream &in)
{
	vector<shared_ptr<Stream>> res;
	try
	{
		unsigned char count = in.ReadByte();
		for (unsigned char i = 0; i < count; i++)
		{
			uint16_t len = (uint16_t)in.ReadInt16();
			BufferInputStream inner = in.GetPartBuffer(len, true);
			shared_ptr<Stream> s = make_shared<Stream>();
			s->id = inner.ReadByte();
			s->type = static_cast<StreamInfo::Type>(inner.ReadByte());
			s->codec = (uint32_t)inner.ReadInt32();
			uint32_t flags = (uint32_t)inner.ReadInt32();
			s->enabled = flags & ExtraStreamFlags::Flags::Enabled;
			s->frameDuration = (uint16_t)inner.ReadInt16();
			res.push_back(s);
		}
	}
	catch (out_of_range &x)
	{
		LOGW("Error deserializing streams: %s", x.what());
	}
	return res;
}

void VoIPGroupController::SetParticipantStreams(int32_t userID, unsigned char *serializedStreams, size_t length)
{
	LOGD("Set participant streams for %d", userID);
	MutexGuard m(participantsMutex);
	for (vector<GroupCallParticipant>::iterator p = participants.begin(); p != participants.end(); ++p)
	{
		if (p->userID == userID)
		{
			BufferInputStream in(serializedStreams, length);
			vector<shared_ptr<Stream>> streams = DeserializeStreams(in);
			for (vector<shared_ptr<Stream>>::iterator ns = streams.begin(); ns != streams.end(); ++ns)
			{
				bool found = false;
				for (vector<shared_ptr<Stream>>::iterator s = p->streams.begin(); s != p->streams.end(); ++s)
				{
					if ((*s)->id == (*ns)->id)
					{
						(*s)->enabled = (*ns)->enabled;
						if (groupCallbacks.participantAudioStateChanged)
							groupCallbacks.participantAudioStateChanged(this, userID, (*s)->enabled);
						found = true;
						break;
					}
				}
				if (!found)
				{
					LOGW("Tried to add stream %d for user %d but adding/removing streams is not supported", (*ns)->id, userID);
				}
			}
			break;
		}
	}
}

size_t VoIPGroupController::GetInitialStreams(unsigned char *buf, size_t size)
{
	BufferOutputStream s(buf, size);
	s.WriteByte(1); // streams count

	s.WriteInt16(12); // this object length
	s.WriteByte(1);   // stream id
	s.WriteByte(StreamInfo::Type::Audio);
	s.WriteInt32(Codec::Opus);
	s.WriteInt32(ExtraStreamFlags::Flags::Enabled | ExtraStreamFlags::Flags::Dtx); // flags
	s.WriteInt16(60);									 // frame duration

	return s.GetLength();
}

void VoIPGroupController::SendInit()
{
	SendRecentPacketsRequest();
}

void VoIPGroupController::ProcessIncomingPacket(NetworkPacket &packet, Endpoint &srcEndpoint)
{
}

void VoIPGroupController::SendUdpPing(Endpoint &endpoint)
{
}

void VoIPGroupController::SetNetworkType(int type)
{
	networkType = type;
	UpdateDataSavingState();
	UpdateAudioBitrateLimit();
	string itfName = udpSocket->GetLocalInterfaceInfo(NULL, NULL);
	if (itfName != activeNetItfName)
	{
		udpSocket->OnActiveInterfaceChanged();
		LOGI("Active network interface changed: %s -> %s", activeNetItfName.c_str(), itfName.c_str());
		bool isFirstChange = activeNetItfName.length() == 0;
		activeNetItfName = itfName;
		if (isFirstChange)
			return;
		udpConnectivityState = UDP_UNKNOWN;
		udpPingCount = 0;
		lastUdpPingTime = 0;
		if (proxyProtocol == PROXY_SOCKS5)
			InitUDPProxy();
		selectCanceller->CancelSelect();
	}
}

void VoIPGroupController::SendRecentPacketsRequest()
{
	BufferOutputStream out(1024);
	out.WriteInt32(TLID_UDP_REFLECTOR_REQUEST_PACKETS_INFO); // TL function
	out.WriteInt32(GetCurrentUnixtime());					 // date:int
	out.WriteInt64(0);										 // query_id:long
	out.WriteInt32(64);										 // recv_num:int
	out.WriteInt32(0);										 // sent_num:int
	SendSpecialReflectorRequest(out.GetBuffer(), out.GetLength());
}

void VoIPGroupController::SendSpecialReflectorRequest(unsigned char *data, size_t len)
{
	/*BufferOutputStream out(1024);
	unsigned char buf[1500];
	crypto.rand_bytes(buf, 8);
	out.WriteBytes(buf, 8);
	out.WriteInt32((int32_t)len);
	out.WriteBytes(data, len);
	if(out.GetLength()%16!=0){
		size_t paddingLen=16-(out.GetLength()%16);
		crypto.rand_bytes(buf, paddingLen);
		out.WriteBytes(buf, paddingLen);
	}
	unsigned char iv[16];
	crypto.rand_bytes(iv, 16);
	unsigned char key[32];
	crypto.sha256(reflectorSelfSecret, 16, key);
	unsigned char _iv[16];
	memcpy(_iv, iv, 16);
	size_t encryptedLen=out.GetLength();
	crypto.aes_cbc_encrypt(out.GetBuffer(), buf, encryptedLen, key, _iv);
	out.Reset();
	out.WriteBytes(reflectorSelfTag, 16);
	out.WriteBytes(iv, 16);
	out.WriteBytes(buf, encryptedLen);
	out.WriteBytes(reflectorSelfSecret, 16);
	crypto.sha256(out.GetBuffer(), out.GetLength(), buf);
	out.Rewind(16);
	out.WriteBytes(buf, 16);

	NetworkPacket pkt={0};
	pkt.address=&groupReflector.address;
	pkt.port=groupReflector.port;
	pkt.protocol=PROTO_UDP;
	pkt.data=out.GetBuffer();
	pkt.length=out.GetLength();
	ActuallySendPacket(pkt, groupReflector);*/
}

void VoIPGroupController::SendRelayPings()
{
	//LOGV("Send relay pings 2");
	double currentTime = GetCurrentTime();
	if (currentTime - groupReflector.lastPingTime >= 0.25)
	{
		SendRecentPacketsRequest();
		groupReflector.lastPingTime = currentTime;
	}
}

void VoIPGroupController::OnAudioOutputReady()
{
	encoder->SetDTX(true);
	audioMixer.SetOutput(audioOutput.get());
	audioMixer.SetEchoCanceller(echoCanceller.get());
	audioMixer.Start();
	audioOutput->Start();
	audioOutStarted = true;
	encoder->SetLevelMeter(selfLevelMeter);
}

void VoIPGroupController::WritePacketHeader(uint32_t seq, BufferOutputStream *s, unsigned char type, uint32_t length, PacketSender *source)
{
	s->WriteInt32(TLID_DECRYPTED_AUDIO_BLOCK);
	int64_t randomID;
	crypto.rand_bytes((uint8_t *)&randomID, 8);
	s->WriteInt64(randomID);
	unsigned char randBytes[7];
	crypto.rand_bytes(randBytes, 7);
	s->WriteByte(7);
	s->WriteBytes(randBytes, 7);
	uint32_t pflags = LEGACY_PFLAG_HAS_SEQ | LEGACY_PFLAG_HAS_SENDER_TAG_HASH;
	if (length > 0)
		pflags |= LEGACY_PFLAG_HAS_DATA;
	pflags |= ((uint32_t)type) << 24;
	s->WriteInt32(pflags);

	if (type == PKT_STREAM_DATA || type == PKT_STREAM_DATA_X2 || type == PKT_STREAM_DATA_X3)
	{
		conctl.PacketSent(seq, length);
	}

	/*if(pflags & LEGACY_PFLAG_HAS_CALL_ID){
		s->WriteBytes(callID, 16);
	}*/
	//s->WriteInt32(lastRemoteSeq);
	s->WriteInt32(seq);
	s->WriteBytes(reflectorSelfTagHash, 16);
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

void VoIPGroupController::SendPacket(unsigned char *data, size_t len, Endpoint &ep, PendingOutgoingPacket &srcPacket)
{
	if (stopping)
		return;
	if (ep.type == Endpoint::Type::TCP_RELAY && !useTCP)
		return;
	BufferOutputStream out(len + 128);
	//LOGV("send group packet %u", len);

	out.WriteBytes(reflectorSelfTag, 16);

	if (len > 0)
	{
		BufferOutputStream inner(len + 128);
		inner.WriteInt32((uint32_t)len);
		inner.WriteBytes(data, len);
		size_t padLen = 16 - inner.GetLength() % 16;
		if (padLen < 12)
			padLen += 16;
		unsigned char padding[28];
		crypto.rand_bytes((uint8_t *)padding, padLen);
		inner.WriteBytes(padding, padLen);
		assert(inner.GetLength() % 16 == 0);

		unsigned char key[32], iv[32], msgKey[16];
		out.WriteBytes(keyFingerprint, 8);
		BufferOutputStream buf(len + 32);
		size_t x = 0;
		buf.WriteBytes(encryptionKey + 88 + x, 32);
		buf.WriteBytes(inner.GetBuffer() + 4, inner.GetLength() - 4);
		unsigned char msgKeyLarge[32];
		crypto.sha256(buf.GetBuffer(), buf.GetLength(), msgKeyLarge);
		memcpy(msgKey, msgKeyLarge + 8, 16);
		KDF2(msgKey, 0, key, iv);
		out.WriteBytes(msgKey, 16);
		//LOGV("<- MSG KEY: %08x %08x %08x %08x, hashed %u", *reinterpret_cast<int32_t*>(msgKey), *reinterpret_cast<int32_t*>(msgKey+4), *reinterpret_cast<int32_t*>(msgKey+8), *reinterpret_cast<int32_t*>(msgKey+12), inner.GetLength()-4);

		unsigned char aesOut[MSC_STACK_FALLBACK(inner.GetLength(), 1500)];
		crypto.aes_ige_encrypt(inner.GetBuffer(), aesOut, inner.GetLength(), key, iv);
		out.WriteBytes(aesOut, inner.GetLength());
	}

	// relay signature
	out.WriteBytes(reflectorSelfSecret, 16);
	unsigned char sig[32];
	crypto.sha256(out.GetBuffer(), out.GetLength(), sig);
	out.Rewind(16);
	out.WriteBytes(sig, 16);

	if (srcPacket.type == PKT_STREAM_DATA || srcPacket.type == PKT_STREAM_DATA_X2 || srcPacket.type == PKT_STREAM_DATA_X3)
	{
		PacketIdMapping mapping = {srcPacket.seq, *reinterpret_cast<uint16_t *>(sig + 14), 0};
		MutexGuard m(sentPacketsMutex);
		recentSentPackets.push_back(mapping);
		//LOGD("sent packet with id: %04X", mapping.id);
		while (recentSentPackets.size() > 64)
			recentSentPackets.erase(recentSentPackets.begin());
	}
	packetManager.setLastSentSeq(srcPacket.seq);

	if (IS_MOBILE_NETWORK(networkType))
		stats.bytesSentMobile += (uint64_t)out.GetLength();
	else
		stats.bytesSentWifi += (uint64_t)out.GetLength();

	/*NetworkPacket pkt={0};
	pkt.address=(NetworkAddress*)&ep.address;
	pkt.port=ep.port;
	pkt.length=out.GetLength();
	pkt.data=out.GetBuffer();
	pkt.protocol=ep.type==Endpoint::Type::TCP_RELAY ? PROTO_TCP : PROTO_UDP;
	ActuallySendPacket(pkt, ep);*/
}

void VoIPGroupController::SetCallbacks(VoIPGroupController::Callbacks callbacks)
{
	VoIPController::SetCallbacks(callbacks);
	this->groupCallbacks = callbacks;
}

int32_t VoIPGroupController::GetCurrentUnixtime()
{
	return time(NULL) + timeDifference;
}

float VoIPGroupController::GetParticipantAudioLevel(int32_t userID)
{
	if (userID == userSelfID)
		return selfLevelMeter->GetLevel();
	MutexGuard m(participantsMutex);
	for (vector<GroupCallParticipant>::iterator p = participants.begin(); p != participants.end(); ++p)
	{
		if (p->userID == userID)
		{
			return p->levelMeter->GetLevel();
		}
	}
	return 0;
}

void VoIPGroupController::SetMicMute(bool mute)
{
	micMuted = mute;
	if (audioInput)
	{
		if (mute)
			audioInput->Stop();
		else
			audioInput->Start();
		if (!audioInput->IsInitialized())
		{
			lastError = ERROR_AUDIO_IO;
			SetState(STATE_FAILED);
			return;
		}
	}
	outgoingStreams[0]->enabled = !mute;
	SerializeAndUpdateOutgoingStreams();
}

void VoIPGroupController::SetParticipantVolume(int32_t userID, float volume)
{
	MutexGuard m(participantsMutex);
	for (vector<GroupCallParticipant>::iterator p = participants.begin(); p != participants.end(); ++p)
	{
		if (p->userID == userID)
		{
			for (vector<shared_ptr<Stream>>::iterator s = p->streams.begin(); s != p->streams.end(); ++s)
			{
				if ((*s)->type == StreamInfo::Type::Audio)
				{
					if ((*s)->decoder)
					{
						float db;
						if (volume == 0.0f)
							db = -INFINITY;
						else if (volume < 1.0f)
							db = -50.0f * (1.0f - volume);
						else if (volume > 1.0f && volume <= 2.0f)
							db = 10.0f * (volume - 1.0f);
						else
							db = 0.0f;
						//LOGV("Setting user %u audio volume to %.2f dB", userID, db);
						audioMixer.SetInputVolume((*s)->callbackWrapper, db);
					}
					break;
				}
			}
			break;
		}
	}
}

void VoIPGroupController::SerializeAndUpdateOutgoingStreams()
{
	BufferOutputStream out(1024);
	out.WriteByte((unsigned char)outgoingStreams.size());

	for (vector<shared_ptr<Stream>>::iterator s = outgoingStreams.begin(); s != outgoingStreams.end(); ++s)
	{
		BufferOutputStream o(128);
		o.WriteByte((*s)->id);
		o.WriteByte((*s)->type);
		o.WriteInt32((*s)->codec);
		o.WriteInt32((unsigned char)(((*s)->enabled ? ExtraStreamFlags::Flags::Enabled : 0) | ExtraStreamFlags::Flags::Dtx));
		o.WriteInt16((*s)->frameDuration);
		out.WriteInt16((int16_t)o.GetLength());
		out.WriteBytes(o.GetBuffer(), o.GetLength());
	}
	if (groupCallbacks.updateStreams)
		groupCallbacks.updateStreams(this, out.GetBuffer(), out.GetLength());
}

std::string VoIPGroupController::GetDebugString()
{
	std::string r = "Remote endpoints: \n";
	char buffer[2048];
	for (pair<const int64_t, Endpoint> &_endpoint : endpoints)
	{
		Endpoint &endpoint = _endpoint.second;
		const char *type;
		switch (endpoint.type)
		{
		case Endpoint::Type::UDP_P2P_INET:
			type = "UDP_P2P_INET";
			break;
		case Endpoint::Type::UDP_P2P_LAN:
			type = "UDP_P2P_LAN";
			break;
		case Endpoint::Type::UDP_RELAY:
			type = "UDP_RELAY";
			break;
		case Endpoint::Type::TCP_RELAY:
			type = "TCP_RELAY";
			break;
		default:
			type = "UNKNOWN";
			break;
		}
		snprintf(buffer, sizeof(buffer), "%s:%u %dms [%s%s]\n", endpoint.address.ToString().c_str(), endpoint.port, (int)(endpoint.averageRTT * 1000), type, currentEndpoint == endpoint.id ? ", IN_USE" : "");
		r += buffer;
	}
	double avgLate[3];
	shared_ptr<JitterBuffer> jitterBuffer = incomingStreams.size() == 1 ? incomingStreams[0]->jitterBuffer : NULL;
	if (jitterBuffer)
		jitterBuffer->GetAverageLateCount(avgLate);
	else
		memset(avgLate, 0, 3 * sizeof(double));
	snprintf(buffer, sizeof(buffer),
			 "RTT avg/min: %d/%d\n"
			 "Congestion window: %d/%d bytes\n"
			 "Key fingerprint: %02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX\n"
			 "Last sent/ack'd seq: %u/%u\n"
			 "Send/recv losses: %u/%u (%d%%)\n"
			 "Audio bitrate: %d kbit\n"
			 "Bytes sent/recvd: %llu/%llu\n\n",
			 (int)(conctl.GetAverageRTT() * 1000), (int)(conctl.GetMinimumRTT() * 1000),
			 int(conctl.GetInflightDataSize()), int(conctl.GetCongestionWindow()),
			 keyFingerprint[0], keyFingerprint[1], keyFingerprint[2], keyFingerprint[3], keyFingerprint[4], keyFingerprint[5], keyFingerprint[6], keyFingerprint[7],
			 getBestPacketManager().getLastSentSeq(), getBestPacketManager().getLastAckedSeq(),
			 conctl.GetSendLossCount(), recvLossCount, encoder ? encoder->GetPacketLoss() : 0,
			 encoder ? (encoder->GetBitrate() / 1000) : 0,
			 (long long unsigned int)(stats.bytesSentMobile + stats.bytesSentWifi),
			 (long long unsigned int)(stats.bytesRecvdMobile + stats.bytesRecvdWifi));

	MutexGuard m(participantsMutex);
	for (vector<GroupCallParticipant>::iterator p = participants.begin(); p != participants.end(); ++p)
	{
		snprintf(buffer, sizeof(buffer), "Participant id: %d\n", p->userID);
		r += buffer;
		for (vector<shared_ptr<Stream>>::iterator stm = p->streams.begin(); stm != p->streams.end(); ++stm)
		{
			char *codec = reinterpret_cast<char *>(&(*stm)->codec);
			snprintf(buffer, sizeof(buffer), "Stream %d (type %d, codec '%c%c%c%c', %sabled)\n",
					 (*stm)->id, (*stm)->type, codec[3], codec[2], codec[1], codec[0], (*stm)->enabled ? "en" : "dis");
			r += buffer;
			if ((*stm)->enabled)
			{
				if ((*stm)->jitterBuffer)
				{
					snprintf(buffer, sizeof(buffer), "Jitter buffer: %d/%.2f\n",
							 (*stm)->jitterBuffer->GetMinPacketCount(), (*stm)->jitterBuffer->GetAverageDelay());
					r += buffer;
				}
			}
		}
		r += "\n";
	}
	return r;
}
