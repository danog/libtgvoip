//
// Created by Grishka on 29.03.17.
//

#ifndef LIBTGVOIP_NETWORKSOCKET_H
#define LIBTGVOIP_NETWORKSOCKET_H

#include "tools/Buffers.h"
#include "tools/utils.h"
#include <atomic>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

namespace tgvoip
{

// API compatibility
struct IPv4Address
{
    IPv4Address(std::string addr) : addr(addr){};
    std::string addr;
};
struct IPv6Address
{
    IPv6Address(std::string addr) : addr(addr){};
    std::string addr;
};

enum class NetworkProtocol
{
    UDP = 0,
    TCP
};

struct TCPO2State
{
    unsigned char key[32];
    unsigned char iv[16];
    unsigned char ecount[16];
    uint32_t num;
};

class NetworkAddress
{
public:
    virtual std::string ToString() const;
    bool operator==(const NetworkAddress &other) const;
    bool operator!=(const NetworkAddress &other) const;
    virtual ~NetworkAddress() = default;
    virtual bool IsEmpty() const;
    virtual bool PrefixMatches(const unsigned int prefix, const NetworkAddress &other) const;

    static NetworkAddress Empty();
    static NetworkAddress IPv4(std::string str);
    static NetworkAddress IPv4(uint32_t addr);
    static NetworkAddress IPv4(const BufferInputStream &in);
    static NetworkAddress IPv6(std::string str);
    static NetworkAddress IPv6(const uint8_t addr[16]);
    static NetworkAddress IPv6(const BufferInputStream &in);

    bool isIPv6 = false;
    union {
        uint32_t ipv4;
        uint8_t ipv6[16];
    } addr;

public:
    NetworkAddress(){};
};

struct NetworkPacket
{
    TGVOIP_MOVE_ONLY(NetworkPacket);
    std::shared_ptr<Buffer> data;
    NetworkAddress address;
    uint16_t port;
    NetworkProtocol protocol;

    static NetworkPacket Empty()
    {
        return NetworkPacket{std::make_shared<Buffer>(), NetworkAddress::Empty(), 0, NetworkProtocol::UDP};
    }

    bool IsEmpty()
    {
        return !data || data->IsEmpty() || (protocol == NetworkProtocol::UDP && (port == 0 || address.IsEmpty()));
    }
};

class SocketSelectCanceller
{
public:
    virtual ~SocketSelectCanceller();
    virtual void CancelSelect() = 0;
    static std::unique_ptr<SocketSelectCanceller> Create();
};

class NetworkSocket
{
public:
    friend class NetworkSocketPosix;
    friend class NetworkSocketWinsock;

    TGVOIP_DISALLOW_COPY_AND_ASSIGN(NetworkSocket);
    NetworkSocket(NetworkProtocol protocol);
    virtual ~NetworkSocket();
    virtual void Send(NetworkPacket &&packet) = 0;
    virtual NetworkPacket Receive(size_t maxLen = 0) = 0;
    size_t Receive(unsigned char *buffer, size_t len);
    virtual void Open() = 0;
    virtual void Close() = 0;
    virtual uint16_t GetLocalPort() { return 0; };
    virtual void Connect(const NetworkAddress address, uint16_t port) = 0;
    virtual std::string GetLocalInterfaceInfo(NetworkAddress *inet4addr, NetworkAddress *inet6addr);
    virtual void OnActiveInterfaceChanged(){};
    virtual NetworkAddress GetConnectedAddress() { return NetworkAddress::Empty(); };
    virtual uint16_t GetConnectedPort() { return 0; };
    virtual void SetTimeouts(int sendTimeout, int recvTimeout){};

    virtual bool IsFailed();
    virtual bool IsReadyToSend()
    {
        return readyToSend;
    }
    virtual bool OnReadyToSend()
    {
        readyToSend = true;
        return true;
    };
    virtual bool OnReadyToReceive() { return true; };
    void SetTimeout(double timeout)
    {
        this->timeout = timeout;
    };

    static std::shared_ptr<NetworkSocket> Create(NetworkProtocol protocol);
    static NetworkAddress ResolveDomainName(std::string name);
    static bool Select(std::vector<std::shared_ptr<NetworkSocket>> &readFds, std::vector<std::shared_ptr<NetworkSocket>> &writeFds, std::vector<std::shared_ptr<NetworkSocket>> &errorFds, const std::unique_ptr<SocketSelectCanceller> &canceller);

protected:
    virtual uint16_t GenerateLocalPort();
    virtual void SetMaxPriority();

    static void GenerateTCPO2States(unsigned char *buffer, TCPO2State *recvState, TCPO2State *sendState);
    static void EncryptForTCPO2(unsigned char *buffer, size_t len, TCPO2State *state);
    double ipv6Timeout;
    unsigned char nat64Prefix[12];
    std::atomic<bool> failed;
    bool readyToSend = false;
    double lastSuccessfulOperationTime = 0.0;
    double timeout = 0.0;
    NetworkProtocol protocol;
};

class NetworkSocketWrapper : public NetworkSocket
{
public:
    NetworkSocketWrapper(NetworkProtocol protocol) : NetworkSocket(protocol){};
    virtual ~NetworkSocketWrapper(){};
    virtual std::shared_ptr<NetworkSocket> GetWrapped() = 0;
    virtual void InitConnection() = 0;
    virtual void SetNonBlocking(bool){};
};

class NetworkSocketTCPObfuscated : public NetworkSocketWrapper
{
public:
    NetworkSocketTCPObfuscated(const std::shared_ptr<NetworkSocket> &wrapped);
    virtual ~NetworkSocketTCPObfuscated();
    virtual std::shared_ptr<NetworkSocket> GetWrapped();
    virtual void InitConnection();
    virtual void Send(NetworkPacket &&packet) override;
    virtual NetworkPacket Receive(size_t maxLen) override;
    virtual void Open();
    virtual void Close();
    virtual void Connect(const NetworkAddress address, uint16_t port);
    virtual bool OnReadyToSend();

    virtual bool IsFailed();
    virtual bool IsReadyToSend()
    {
        return readyToSend && wrapped->IsReadyToSend();
    };

private:
    std::shared_ptr<NetworkSocket> wrapped;
    TCPO2State recvState;
    TCPO2State sendState;
    bool initialized = false;
};

class NetworkSocketSOCKS5Proxy : public NetworkSocketWrapper
{
public:
    NetworkSocketSOCKS5Proxy(const std::shared_ptr<NetworkSocket> &tcp, const std::shared_ptr<NetworkSocket> &udp, std::string username, std::string password);
    virtual ~NetworkSocketSOCKS5Proxy();
    virtual void Send(NetworkPacket &&packet) override;
    virtual NetworkPacket Receive(size_t maxLen) override;
    virtual void Open() override;
    virtual void Close();
    virtual void Connect(const NetworkAddress address, uint16_t port);
    virtual std::shared_ptr<NetworkSocket> GetWrapped();
    virtual void InitConnection();
    virtual bool IsFailed();
    virtual NetworkAddress GetConnectedAddress();
    virtual uint16_t GetConnectedPort();
    virtual bool OnReadyToSend();
    virtual bool OnReadyToReceive();

    bool NeedSelectForSending();

private:
    void SendConnectionCommand();
    enum ConnectionState
    {
        Initial,
        WaitingForAuthMethod,
        WaitingForAuthResult,
        WaitingForCommandResult,
        Connected
    };
    std::shared_ptr<NetworkSocket> tcp;
    std::shared_ptr<NetworkSocket> udp;
    std::string username;
    std::string password;
    NetworkAddress connectedAddress = NetworkAddress::Empty();
    uint16_t connectedPort;
    ConnectionState state = ConnectionState::Initial;
};

} // namespace tgvoip

#endif //LIBTGVOIP_NETWORKSOCKET_H
