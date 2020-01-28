//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#pragma once

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#include "os/darwin/AudioUnitIO.h"
#endif
#include <stdint.h>
#include <vector>
#include <deque>
#include <fstream>
#include <iomanip>
#include <string>
#include <unordered_map>
#include <map>
#include <memory>
#include "video/VideoSource.h"
#include "video/VideoRenderer.h"
#include <atomic>
#include "video/ScreamCongestionController.h"
#include "audio/AudioInput.h"
#include "audio/Device.h"
#include "tools/BlockingQueue.h"
#include "audio/AudioOutput.h"
#include "audio/AudioIO.h"
#include "controller/net/JitterBuffer.h"
#include "controller/net/Endpoint.h"
#include "controller/audio/OpusDecoder.h"
#include "controller/audio/OpusEncoder.h"
#include "controller/audio/EchoCanceller.h"
#include "controller/net/CongestionControl.h"
#include "controller/protocol/Ack.h"
#include "controller/protocol/PacketStructs.h"
#include "tools/Buffers.h"
#include "controller/net/PacketReassembler.h"
#include "tools/MessageThread.h"
#include "tools/utils.h"
#include "controller/PrivateDefines.h"

#if defined HAVE_CONFIG_H || defined TGVOIP_USE_INSTALLED_OPUS
#include <opus/opus.h>
#else
#include <opus/opus.h>
#endif

#define LIBTGVOIP_VERSION "2.5"

#ifdef _WIN32
#undef GetCurrentTime
#undef ERROR_TIMEOUT
#endif

#define ENFORCE_MSG_THREAD assert(messageThread.IsCurrent())

#define TGVOIP_PEER_CAP_GROUP_CALLS 1
#define TGVOIP_PEER_CAP_VIDEO_CAPTURE 2
#define TGVOIP_PEER_CAP_VIDEO_DISPLAY 4

namespace tgvoip
{

enum
{
    PROXY_NONE = 0,
    PROXY_SOCKS5,
    //PROXY_HTTP
};

enum
{
    STATE_WAIT_INIT = 1,
    STATE_WAIT_INIT_ACK,
    STATE_ESTABLISHED,
    STATE_FAILED,
    STATE_RECONNECTING
};

enum
{
    ERROR_UNKNOWN = 0,
    ERROR_INCOMPATIBLE,
    ERROR_TIMEOUT,
    ERROR_AUDIO_IO,
    ERROR_PROXY
};

enum
{
    NET_TYPE_UNKNOWN = 0,
    NET_TYPE_GPRS,
    NET_TYPE_EDGE,
    NET_TYPE_3G,
    NET_TYPE_HSPA,
    NET_TYPE_LTE,
    NET_TYPE_WIFI,
    NET_TYPE_ETHERNET,
    NET_TYPE_OTHER_HIGH_SPEED,
    NET_TYPE_OTHER_LOW_SPEED,
    NET_TYPE_DIALUP,
    NET_TYPE_OTHER_MOBILE
};

enum
{
    DATA_SAVING_NEVER = 0,
    DATA_SAVING_MOBILE,
    DATA_SAVING_ALWAYS
};

struct CryptoFunctions
{
    void (*rand_bytes)(uint8_t *buffer, size_t length);
    void (*sha1)(uint8_t *msg, size_t length, uint8_t *output);
    void (*sha256)(uint8_t *msg, size_t length, uint8_t *output);
    void (*aes_ige_encrypt)(uint8_t *in, uint8_t *out, size_t length, uint8_t *key, uint8_t *iv);
    void (*aes_ige_decrypt)(uint8_t *in, uint8_t *out, size_t length, uint8_t *key, uint8_t *iv);
    void (*aes_ctr_encrypt)(uint8_t *inout, size_t length, uint8_t *key, uint8_t *iv, uint8_t *ecount, uint32_t *num);
    void (*aes_cbc_encrypt)(uint8_t *in, uint8_t *out, size_t length, uint8_t *key, uint8_t *iv);
    void (*aes_cbc_decrypt)(uint8_t *in, uint8_t *out, size_t length, uint8_t *key, uint8_t *iv);
};

struct CellularCarrierInfo
{
    std::string name;
    std::string mcc;
    std::string mnc;
    std::string countryCode;
};

class PacketSender;

class VoIPController : Ack
{
    friend class VoIPGroupController;
    friend class PacketSender;

public:
    TGVOIP_DISALLOW_COPY_AND_ASSIGN(VoIPController);
    struct Config
    {
        Config(double initTimeout = 30.0, double recvTimeout = 20.0, int dataSaving = DATA_SAVING_NEVER, bool enableAEC = false, bool enableNS = false, bool enableAGC = false, bool enableCallUpgrade = false)
        {
            this->initTimeout = initTimeout;
            this->recvTimeout = recvTimeout;
            this->dataSaving = dataSaving;
            this->enableAEC = enableAEC;
            this->enableNS = enableNS;
            this->enableAGC = enableAGC;
            this->enableCallUpgrade = enableCallUpgrade;
        }

        double initTimeout;
        double recvTimeout;
        int dataSaving;
#ifndef _WIN32
        std::string logFilePath = "";
        std::string statsDumpFilePath = "";
#else
        std::wstring logFilePath = L"";
        std::wstring statsDumpFilePath = L"";
#endif

        bool enableAEC;
        bool enableNS;
        bool enableAGC;

        bool enableCallUpgrade;

        bool logPacketStats = false;
        bool enableVolumeControl = false;

        bool enableVideoSend = false;
        bool enableVideoReceive = false;
    };

    struct TrafficStats
    {
        uint64_t bytesSentWifi = 0;
        uint64_t bytesRecvdWifi = 0;
        uint64_t bytesSentMobile = 0;
        uint64_t bytesRecvdMobile = 0;
    };

    VoIPController();
    virtual ~VoIPController();

    /**
      * Set the initial endpoints (relays)
      * @param endpoints Endpoints converted from phone.PhoneConnection TL objects
      * @param allowP2p Whether p2p connectivity is allowed
      * @param connectionMaxLayer The max_layer field from the phoneCallProtocol object returned by Telegram server.
      * DO NOT HARDCODE THIS VALUE, it's extremely important for backwards compatibility.
      */
    void SetRemoteEndpoints(std::vector<Endpoint> endpoints, bool allowP2p, int32_t connectionMaxLayer);
    /**
      * Initialize and start all the internal threads
      */
    void Start();
    /**
      * Stop any internal threads. Don't call any other methods after this.
      */
    void Stop();
    /**
      * Initiate connection
      */
    void Connect();
    Endpoint &GetRemoteEndpoint();
    /**
      * Get the debug info string to be displayed in client UI
      */
    virtual std::string GetDebugString();
    /**
      * Notify the library of network type change
      * @param type The new network type
      */
    virtual void SetNetworkType(int type);
    /**
      * Get the average round-trip time for network packets
      * @return
      */
    double GetAverageRTT();
    static double GetCurrentTime();
    /**
      * Use this field to store any of your context data associated with this call
      */
    void *implData;
    /**
      *
      * @param mute
      */
    virtual void SetMicMute(bool mute);
    /**
      *
      * @param key
      * @param isOutgoing
      */
    void SetEncryptionKey(std::vector<uint8_t>, bool isOutgoing);
    /**
      *
      * @param cfg
      */
    void SetConfig(const Config &cfg);
    /**
      *
      * @param stats
      */
    void GetStats(TrafficStats *stats);
    /**
      *
      * @return
      */
    int64_t GetPreferredRelayID();
    /**
      *
      * @return
      */
    int GetLastError();
    /**
      *
      */
    static CryptoFunctions crypto;
    /**
      *
      * @return
      */
    static const char *GetVersion();
    /**
      *
      * @return
      */
    std::string GetDebugLog();
    /**
      *
      * @return
      */
    static std::vector<AudioInputDevice> EnumerateAudioInputs();
    /**
      *
      * @return
      */
    static std::vector<AudioOutputDevice> EnumerateAudioOutputs();
    /**
      *
      * @param id
      */
    void SetCurrentAudioInput(std::string id);
    /**
      *
      * @param id
      */
    void SetCurrentAudioOutput(std::string id);
    /**
      *
      * @return
      */
    std::string GetCurrentAudioInputID();
    /**
      *
      * @return
      */
    std::string GetCurrentAudioOutputID();
    /**
      * Set the proxy server to route the data through. Call this before connecting.
      * @param protocol PROXY_NONE or PROXY_SOCKS5
      * @param address IP address or domain name of the server
      * @param port Port of the server
      * @param username Username; empty string for anonymous
      * @param password Password; empty string if none
      */
    void SetProxy(int protocol, std::string address, uint16_t port, std::string username, std::string password);
    /**
      * Get the number of signal bars to display in the client UI.
      * @return the number of signal bars, from 1 to 4
      */
    int GetSignalBarsCount();
    /**
      * Enable or disable AGC (automatic gain control) on audio output. Should only be enabled on phones when the earpiece speaker is being used.
      * The audio output will be louder with this on.
      * AGC with speakerphone or other kinds of loud speakers has detrimental effects on some echo cancellation implementations.
      * @param enabled I usually pick argument names to be self-explanatory
      */
    void SetAudioOutputGainControlEnabled(bool enabled);
    /**
      * Get the additional capabilities of the peer client app
      * @return corresponding TGVOIP_PEER_CAP_* flags OR'ed together
      */
    uint32_t GetPeerCapabilities();
    /**
      * Send the peer the key for the group call to prepare this private call to an upgrade to a E2E group call.
      * The peer must have the TGVOIP_PEER_CAP_GROUP_CALLS capability. After the peer acknowledges the key, Callbacks::groupCallKeySent will be called.
      * @param key newly-generated group call key, must be exactly 265 bytes long
      */
    void SendGroupCallKey(uint8_t *key);
    /**
      * In an incoming call, request the peer to generate a new encryption key, send it to you and upgrade this call to a E2E group call.
      */
    void RequestCallUpgrade();
    void SetEchoCancellationStrength(int strength);
    int GetConnectionState();
    bool NeedRate();
    /**
      * Get the maximum connection layer supported by this libtgvoip version.
      * Pass this as <code>max_layer</code> in the phone.phoneConnection TL object when requesting and accepting calls.
      */
    static const int32_t GetConnectionMaxLayer()
    {
        return 92;
    };
    /**
      * Get the persistable state of the library, like proxy capabilities, to save somewhere on the disk. Call this at the end of the call.
      * Using this will speed up the connection establishment in some cases.
      */
    std::vector<uint8_t> GetPersistentState();
    /**
      * Load the persistable state. Call this before starting the call.
      */
    void SetPersistentState(std::vector<uint8_t> state);

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
    void SetAudioDataCallbacks(std::function<void(int16_t *, size_t)> input, std::function<void(int16_t *, size_t)> output, std::function<void(int16_t *, size_t)> preprocessed);
#endif

    void SetVideoCodecSpecificData(const std::vector<Buffer> &data);

    struct Callbacks
    {
        void (*connectionStateChanged)(VoIPController *, int);
        void (*signalBarCountChanged)(VoIPController *, int);
        void (*groupCallKeySent)(VoIPController *);
        void (*groupCallKeyReceived)(VoIPController *, const uint8_t *);
        void (*upgradeToGroupCallRequested)(VoIPController *);
    };
    void SetCallbacks(Callbacks callbacks);

    float GetOutputLevel()
    {
        return 0.0f;
    };
    void SetVideoSource(video::VideoSource *source);
    void SetVideoRenderer(video::VideoRenderer *renderer);

    void SetInputVolume(float level);
    void SetOutputVolume(float level);
#if defined(__APPLE__) && defined(TARGET_OS_OSX)
    void SetAudioOutputDuckingEnabled(bool enabled);
#endif

    enum StreamType
    {
        STREAM_TYPE_AUDIO = 1,
        STREAM_TYPE_VIDEO
    };

    struct Stream : Ack
    {
        int32_t userID;
        uint8_t id;
        StreamType type;
        uint32_t codec;
        bool enabled;
        bool extraECEnabled;
        uint16_t frameDuration;
        std::shared_ptr<JitterBuffer> jitterBuffer;
        std::shared_ptr<OpusDecoder> decoder;
        std::shared_ptr<PacketReassembler> packetReassembler;
        std::shared_ptr<CallbackWrapper> callbackWrapper;
        std::unique_ptr<PacketSender> packetSender;
        std::vector<Buffer> codecSpecificData;
        bool csdIsValid = false;
        bool paused = false;
        int resolution;

        unsigned int width = 0;
        unsigned int height = 0;
        uint16_t rotation = 0;
    };

    struct ProtocolInfo
    {
        uint32_t version;
        uint32_t maxVideoResolution;
        std::vector<uint32_t> videoDecoders;
        bool videoCaptureSupported;
        bool videoDisplaySupported;
        bool callUpgradeSupported;
    };

protected:
    virtual void ProcessIncomingPacket(NetworkPacket &packet, Endpoint &srcEndpoint);
    virtual void ProcessExtraData(Buffer &data);
    virtual void WritePacketHeader(uint32_t seq, BufferOutputStream *s, unsigned char type, uint32_t length, PacketSender *source);
    virtual void SendPacket(unsigned char *data, size_t len, Endpoint &ep, PendingOutgoingPacket &srcPacket);
    virtual void SendInit();
    virtual void SendUdpPing(Endpoint &endpoint);
    virtual void SendRelayPings();
    virtual void OnAudioOutputReady();
    virtual void SendExtra(Buffer &data, unsigned char type);
    void SendStreamFlags(Stream &stream);
    void InitializeTimers();
    void ResetEndpointPingStats();
    void SendVideoFrame(const Buffer &frame, uint32_t flags, uint32_t rotation);
    void ProcessIncomingVideoFrame(Buffer frame, uint32_t pts, bool keyframe, uint16_t rotation);
    std::shared_ptr<Stream> GetStreamByType(StreamType type, bool outgoing);
    std::shared_ptr<Stream> GetStreamByID(unsigned char id, bool outgoing);
    Endpoint *GetEndpointForPacket(const PendingOutgoingPacket &pkt);
    bool SendOrEnqueuePacket(PendingOutgoingPacket pkt, bool enqueue = true, PacketSender *source = NULL);
    CellularCarrierInfo GetCarrierInfo();

private:
    struct RawPendingOutgoingPacket
    {
        TGVOIP_MOVE_ONLY(RawPendingOutgoingPacket);
        NetworkPacket packet;
        std::shared_ptr<NetworkSocket> socket;
    };
    enum
    {
        UDP_UNKNOWN = 0,
        UDP_PING_PENDING,
        UDP_PING_SENT,
        UDP_AVAILABLE,
        UDP_NOT_AVAILABLE,
        UDP_BAD
    };

    void RunRecvThread();
    void RunSendThread();
    void HandleAudioInput(unsigned char *data, size_t len, unsigned char *secondaryData, size_t secondaryLen);
    void UpdateAudioBitrateLimit();
    void SetState(int state);
    void UpdateAudioOutputState();
    void InitUDPProxy();
    void UpdateDataSavingState();

    size_t decryptPacket(unsigned char *buffer, BufferInputStream &in);
    void encryptPacket(unsigned char *data, size_t len, BufferOutputStream &out);

    void KDF(unsigned char *msgKey, size_t x, unsigned char *aesKey, unsigned char *aesIv);
    void KDF2(unsigned char *msgKey, size_t x, unsigned char *aesKey, unsigned char *aesIv);

    void SendPublicEndpointsRequest();
    void SendPublicEndpointsRequest(const Endpoint &relay);
    Endpoint &GetEndpointByType(const Endpoint::Type type);
    void SendPacketReliably(unsigned char type, unsigned char *data, size_t len, double retryInterval, double timeout, uint8_t tries = 0xFF);

    void InitializeAudio();
    void StartAudio();
    void ProcessAcknowledgedOutgoingExtra(UnacknowledgedExtraData &extra);
    void AddIPv6Relays();
    void AddTCPRelays();
    void SendUdpPings();
    void EvaluateUdpPingResults();
    void UpdateRTT();
    void UpdateCongestion();
    void UpdateAudioBitrate();
    void UpdateSignalBars();
    void UpdateReliablePackets();
    void SendNopPacket();
    void TickJitterBufferAndCongestionControl();
    void ResetUdpAvailability();
    inline static std::string NetworkTypeToString(int type)
    {
        switch (type)
        {
        case NET_TYPE_WIFI:
            return "wifi";
        case NET_TYPE_GPRS:
            return "gprs";
        case NET_TYPE_EDGE:
            return "edge";
        case NET_TYPE_3G:
            return "3g";
        case NET_TYPE_HSPA:
            return "hspa";
        case NET_TYPE_LTE:
            return "lte";
        case NET_TYPE_ETHERNET:
            return "ethernet";
        case NET_TYPE_OTHER_HIGH_SPEED:
            return "other_high_speed";
        case NET_TYPE_OTHER_LOW_SPEED:
            return "other_low_speed";
        case NET_TYPE_DIALUP:
            return "dialup";
        case NET_TYPE_OTHER_MOBILE:
            return "other_mobile";
        default:
            return "unknown";
        }
    }

    inline static std::string GetPacketTypeString(unsigned char type)
    {
        switch (type)
        {
        case PKT_INIT:
            return "init";
        case PKT_INIT_ACK:
            return "init_ack";
        case PKT_STREAM_STATE:
            return "stream_state";
        case PKT_STREAM_DATA:
            return "stream_data";
        case PKT_PING:
            return "ping";
        case PKT_PONG:
            return "pong";
        case PKT_LAN_ENDPOINT:
            return "lan_endpoint";
        case PKT_NETWORK_CHANGED:
            return "network_changed";
        case PKT_NOP:
            return "nop";
        case PKT_STREAM_EC:
            return "stream_ec";
        }
        return string("unknown(") + std::to_string(type) + ')';
    }

    // More legacy
    bool legacyParsePacket(BufferInputStream &in, unsigned char &type, uint32_t &ackId, uint32_t &pseq, uint32_t &acks, unsigned char &pflags, size_t &packetInnerLen);
    void legacyWritePacketHeader(uint32_t pseq, uint32_t acks, BufferOutputStream *s, unsigned char type, uint32_t length, PacketSender *source);

    void handleReliablePackets();

    void SetupOutgoingVideoStream();
    bool WasOutgoingPacketAcknowledged(uint32_t seq, bool checkAll = true);
    RecentOutgoingPacket *GetRecentOutgoingPacket(uint32_t seq);
    void NetworkPacketReceived(std::shared_ptr<NetworkPacket> packet);
    void TrySendOutgoingPackets();

    int state = STATE_WAIT_INIT;
    std::map<int64_t, Endpoint> endpoints;
    int64_t currentEndpoint = 0;
    int64_t preferredRelay = 0;
    int64_t peerPreferredRelay = 0;
    std::atomic<bool> runReceiver = ATOMIC_VAR_INIT(false);

    // Acks now handled in Ack

    HistoricBuffer<uint32_t, 10, double> sendLossCountHistory;
    uint32_t audioTimestampIn = 0;
    uint32_t audioTimestampOut = 0;

    std::unique_ptr<OpusEncoder> encoder;
    std::unique_ptr<tgvoip::audio::AudioIO> audioIO;

    // Obtained from audioIO
    std::shared_ptr<tgvoip::audio::AudioInput> audioInput;
    std::shared_ptr<tgvoip::audio::AudioOutput> audioOutput;

    // Shared between encoder and decoder
    std::shared_ptr<EchoCanceller> echoCanceller;

    std::unique_ptr<Thread> recvThread;
    std::unique_ptr<Thread> sendThread;

    std::vector<PendingOutgoingPacket> sendQueue;
    std::atomic<bool> stopping = ATOMIC_VAR_INIT(false);
    bool audioOutStarted = false;
    uint32_t packetsReceived = 0;
    uint32_t recvLossCount = 0;
    uint32_t prevSendLossCount = 0;
    uint32_t firstSentPing;
    HistoricBuffer<double, 32> rttHistory;
    bool waitingForAcks = false;
    int networkType = NET_TYPE_UNKNOWN;
    int dontSendPackets = 0;
    int lastError;
    bool micMuted = false;
    uint32_t maxBitrate;

    //
    std::vector<std::shared_ptr<Stream>> outgoingStreams;
    std::vector<std::shared_ptr<Stream>> incomingStreams;

    unsigned char encryptionKey[256];
    unsigned char keyFingerprint[8];
    unsigned char callID[16];
    double stateChangeTime;
    bool waitingForRelayPeerInfo = false;
    bool allowP2p = true;
    bool dataSavingMode = false;
    bool dataSavingRequestedByPeer = false;
    std::string activeNetItfName;
    double publicEndpointsReqTime = 0;
    std::vector<ReliableOutgoingPacket> reliablePackets;
    double connectionInitTime = 0;
    double lastRecvPacketTime = 0;
    Config config;
    int32_t peerVersion = 0;
    CongestionControl conctl;
    TrafficStats stats;
    bool receivedInit = false;
    bool receivedInitAck = false;
    bool isOutgoing;

    // Might point to the same or different objects
    std::shared_ptr<NetworkSocket> udpSocket;
    std::shared_ptr<NetworkSocket> realUdpSocket;

    std::ofstream statsDump;
    std::string currentAudioInput;
    std::string currentAudioOutput;
    bool useTCP = false;
    bool useUDP = true;
    bool didAddTcpRelays = false;
    std::unique_ptr<SocketSelectCanceller> selectCanceller;
    HistoricBuffer<unsigned char, 4, int> signalBarsHistory;
    bool audioStarted = false;

    int udpConnectivityState = UDP_UNKNOWN;
    double lastUdpPingTime = 0;
    int udpPingCount = 0;
    int echoCancellationStrength = 1;

    int proxyProtocol = PROXY_NONE;
    std::string proxyAddress;
    uint16_t proxyPort = 0;
    std::string proxyUsername;
    std::string proxyPassword;
    NetworkAddress resolvedProxyAddress = NetworkAddress::Empty();

    uint32_t peerCapabilities = 0;
    Callbacks callbacks{0};
    bool didReceiveGroupCallKey = false;
    bool didReceiveGroupCallKeyAck = false;
    bool didSendGroupCallKey = false;
    bool didSendUpgradeRequest = false;
    bool didInvokeUpgradeCallback = false;

    int32_t connectionMaxLayer = 0;
    bool useMTProto2 = false;
    bool setCurrentEndpointToTCP = false;

    std::vector<UnacknowledgedExtraData> currentExtras;
    std::unordered_map<uint8_t, uint64_t> lastReceivedExtrasByType;
    bool useIPv6 = false;
    bool peerIPv6Available = false;
    NetworkAddress myIPv6{NetworkAddress::Empty()};
    bool shittyInternetMode = false;
    uint8_t extraEcLevel = 0;
    std::deque<Buffer> ecAudioPackets;
    bool didAddIPv6Relays = false;
    bool didSendIPv6Endpoint = false;
    int publicEndpointsReqCount = 0;
    bool wasEstablished = false;
    bool receivedFirstStreamPacket = false;
    std::atomic<unsigned int> unsentStreamPackets = ATOMIC_VAR_INIT(0);
    HistoricBuffer<unsigned int, 5> unsentStreamPacketsHistory;
    bool needReInitUdpProxy = true;
    bool needRate = false;
    BufferPool<1024, 32> outgoingAudioBufferPool;
    BlockingQueue<RawPendingOutgoingPacket> rawSendQueue;

    uint32_t initTimeoutID = MessageThread::INVALID_ID;
    uint32_t udpPingTimeoutID = MessageThread::INVALID_ID;

    // Using a shared_ptr is redundant here, but it allows more flexibility in the OpusEncoder API
    std::shared_ptr<effects::Volume> outputVolume = std::make_shared<effects::Volume>();
    std::shared_ptr<effects::Volume> inputVolume = std::make_shared<effects::Volume>();

    std::vector<uint32_t> peerVideoDecoders;

    MessageThread messageThread;

    // Locked whenever the endpoints vector is modified (but not endpoints themselves) and whenever iterated outside of messageThread.
    // After the call is started, only messageThread is allowed to modify the endpoints vector.
    Mutex endpointsMutex;
    // Locked while audio i/o is being initialized and deinitialized so as to allow it to fully initialize before deinitialization begins.
    Mutex audioIOMutex;

#if defined(TGVOIP_USE_CALLBACK_AUDIO_IO)
    std::function<void(int16_t *, size_t)> audioInputDataCallback;
    std::function<void(int16_t *, size_t)> audioOutputDataCallback;
    std::function<void(int16_t *, size_t)> audioPreprocDataCallback;
    std::unique_ptr<::OpusDecoder, decltype(&opus_decoder_destroy)> preprocDecoder{opus_decoder_create(48000, 1, NULL), &opus_decoder_destroy};
    int16_t preprocBuffer[4096];
#endif
#if defined(__APPLE__) && defined(TARGET_OS_OSX)
    bool macAudioDuckingEnabled = true;
#endif

    video::VideoRenderer *videoRenderer = nullptr;
    uint32_t lastReceivedVideoFrameNumber = UINT32_MAX;

    uint32_t sendLosses = 0;
    uint32_t unacknowledgedIncomingPacketCount = 0;

    ProtocolInfo protocolInfo{0};

    /*** debug report problems ***/
    bool wasReconnecting = false;
    bool wasExtraEC = false;
    bool wasEncoderLaggy = false;
    bool wasNetworkHandover = false;

    /*** persistable state values ***/
    bool proxySupportsUDP = true;
    bool proxySupportsTCP = true;
    std::string lastTestedProxyServer = "";

    /*** server config values ***/
    uint32_t maxAudioBitrate;
    uint32_t maxAudioBitrateEDGE;
    uint32_t maxAudioBitrateGPRS;
    uint32_t maxAudioBitrateSaving;
    uint32_t initAudioBitrate;
    uint32_t initAudioBitrateEDGE;
    uint32_t initAudioBitrateGPRS;
    uint32_t initAudioBitrateSaving;
    uint32_t minAudioBitrate;
    uint32_t audioBitrateStepIncr;
    uint32_t audioBitrateStepDecr;
    double relaySwitchThreshold;
    double p2pToRelaySwitchThreshold;
    double relayToP2pSwitchThreshold;
    double reconnectingTimeout;
    uint32_t needRateFlags;
    double rateMaxAcceptableRTT;
    double rateMaxAcceptableSendLoss;
    double packetLossToEnableExtraEC;
    uint32_t maxUnsentStreamPackets;
    uint32_t unackNopThreshold;

public:
#ifdef __APPLE__
    static double machTimebase;
    static uint64_t machTimestart = 0;
#endif
#ifdef _WIN32
    static int64_t win32TimeScale;
    static bool didInitWin32TimeScale;
#endif
};

}; // namespace tgvoip
