#pragma once
#include "VoIPController.h"
#include "controller/net/Endpoint.h"
#include "controller/net/NetworkSocket.h"

namespace tgvoip
{

class VoIPGroupController : public VoIPController
{
public:
    VoIPGroupController(int32_t timeDifference);
    virtual ~VoIPGroupController();
    void SetGroupCallInfo(unsigned char *encryptionKey, unsigned char *reflectorGroupTag, unsigned char *reflectorSelfTag, unsigned char *reflectorSelfSecret, unsigned char *reflectorSelfTagHash, int32_t selfUserID, NetworkAddress reflectorAddress, NetworkAddress reflectorAddressV6, uint16_t reflectorPort);
    void AddGroupCallParticipant(int32_t userID, unsigned char *memberTagHash, unsigned char *serializedStreams, size_t streamsLength);
    void RemoveGroupCallParticipant(int32_t userID);
    float GetParticipantAudioLevel(int32_t userID);
    virtual void SetMicMute(bool mute);
    void SetParticipantVolume(int32_t userID, float volume);
    void SetParticipantStreams(int32_t userID, unsigned char *serializedStreams, size_t length);
    static size_t GetInitialStreams(unsigned char *buf, size_t size);

    struct Callbacks : public VoIPController::Callbacks
    {
        void (*updateStreams)(VoIPGroupController *, unsigned char *, size_t);
        void (*participantAudioStateChanged)(VoIPGroupController *, int32_t, bool);
    };
    void SetCallbacks(Callbacks callbacks);
    virtual std::string GetDebugString();
    virtual void SetNetworkType(int type);

protected:
    virtual void ProcessIncomingPacket(NetworkPacket &packet, Endpoint &srcEndpoint);
    virtual void SendInit();
    virtual void SendUdpPing(Endpoint &endpoint);
    virtual void SendRelayPings();
    virtual void SendPacket(unsigned char *data, size_t len, Endpoint &ep, PendingOutgoingPacket &srcPacket);
    virtual void WritePacketHeader(uint32_t seq, BufferOutputStream *s, unsigned char type, uint32_t length, PacketSender *sender = NULL);
    virtual void OnAudioOutputReady();

private:
    int32_t GetCurrentUnixtime();
    std::vector<std::shared_ptr<Stream>> DeserializeStreams(BufferInputStream &in);
    void SendRecentPacketsRequest();
    void SendSpecialReflectorRequest(unsigned char *data, size_t len);
    void SerializeAndUpdateOutgoingStreams();
    struct GroupCallParticipant
    {
        int32_t userID;
        unsigned char memberTagHash[32];
        std::vector<std::shared_ptr<Stream>> streams;
        AudioLevelMeter *levelMeter;
    };
    std::vector<GroupCallParticipant> participants;
    unsigned char reflectorSelfTag[16];
    unsigned char reflectorSelfSecret[16];
    unsigned char reflectorSelfTagHash[32];
    int32_t userSelfID;
    Endpoint groupReflector;
    AudioMixer *audioMixer;
    AudioLevelMeter selfLevelMeter;
    Callbacks groupCallbacks;
    struct PacketIdMapping
    {
        uint32_t seq;
        uint16_t id;
        double ackTime;
    };
    std::vector<PacketIdMapping> recentSentPackets;
    Mutex sentPacketsMutex;
    Mutex participantsMutex;
    int32_t timeDifference;
};

} // namespace tgvoip