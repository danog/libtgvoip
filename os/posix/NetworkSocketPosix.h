//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//

#ifndef LIBTGVOIP_NETWORKSOCKETPOSIX_H
#define LIBTGVOIP_NETWORKSOCKETPOSIX_H

#include "../../controller/net/NetworkSocket.h"
#include "../../tools/Buffers.h"
#include <vector>
#include <mutex>
#include <sys/select.h>
#include <pthread.h>

namespace tgvoip
{

class SocketSelectCancellerPosix : public SocketSelectCanceller
{
	friend class NetworkSocketPosix;

public:
	SocketSelectCancellerPosix();
	virtual ~SocketSelectCancellerPosix();
	virtual void CancelSelect();

private:
	int pipeRead;
	int pipeWrite;
};

class NetworkSocketPosix : public NetworkSocket
{
public:
	NetworkSocketPosix(NetworkProtocol protocol);
	virtual ~NetworkSocketPosix() override;
	virtual void Send(NetworkPacket &&packet) override;
	virtual NetworkPacket Receive(size_t maxLen) override;
	virtual void Open() override;
	virtual void Close() override;
	virtual void Connect(const NetworkAddress address, uint16_t port) override;
	virtual std::string GetLocalInterfaceInfo(NetworkAddress *v4addr, NetworkAddress *v6addr) override;
	virtual void OnActiveInterfaceChanged() override;
	virtual uint16_t GetLocalPort() override;

	static std::string V4AddressToString(uint32_t address);
	static std::string V6AddressToString(const unsigned char address[16]);
	static uint32_t StringToV4Address(std::string address);
	static void StringToV6Address(std::string address, unsigned char *out);
	static NetworkAddress ResolveDomainName(std::string name);
	static bool Select(std::vector<std::shared_ptr<NetworkSocket>> &readFds, std::vector<std::shared_ptr<NetworkSocket>> &writeFds, std::vector<std::shared_ptr<NetworkSocket>> &errorFds, const std::unique_ptr<SocketSelectCanceller> &canceller);

	virtual NetworkAddress GetConnectedAddress() override;

	virtual uint16_t GetConnectedPort() override;

	virtual void SetTimeouts(int sendTimeout, int recvTimeout) override;
	virtual bool OnReadyToSend() override;

protected:
	virtual void SetMaxPriority() override;

private:
	static int GetDescriptorFromSocket(NetworkSocket *socket);
	static int GetDescriptorFromSocket(const std::shared_ptr<NetworkSocket> &socket);
	std::atomic<int> fd;
	std::mutex m_fd;
	bool needUpdateNat64Prefix;
	bool nat64Present;
	double switchToV6at;
	std::atomic<bool> isV4Available;
	std::atomic<bool> closing;
	NetworkAddress tcpConnectedAddress = NetworkAddress::Empty();
	uint16_t tcpConnectedPort;
	NetworkPacket pendingOutgoingPacket = NetworkPacket::Empty();
	Buffer recvBuffer = Buffer(2048);
};

} // namespace tgvoip

#endif //LIBTGVOIP_NETWORKSOCKETPOSIX_H
