//
// libtgvoip is free and unencumbered public domain software.
// For more information, see http://unlicense.org or the UNLICENSE file
// you should have received with this source code distribution.
//
#include "Endpoint.h"
#include "../../VoIPServerConfig.h"
#include "../PrivateDefines.h"
using namespace tgvoip;

Endpoint::Endpoint(int64_t id, uint16_t port, const IPv4Address &_address, const IPv6Address &_v6address, Type type, unsigned char peerTag[16]) : address(NetworkAddress::IPv4(_address.addr)), v6address(NetworkAddress::IPv6(_v6address.addr))
{
	this->id = id;
	this->port = port;
	this->type = type;
	memcpy(this->peerTag, peerTag, 16);
	if (type == Type::UDP_RELAY && ServerConfig::GetSharedInstance()->GetBoolean("force_tcp", false))
		this->type = Type::TCP_RELAY;

	lastPingSeq = 0;
	lastPingTime = 0;
	averageRTT = 0;
	socket = NULL;
	udpPongCount = 0;
}

Endpoint::Endpoint(int64_t id, uint16_t port, const NetworkAddress _address, const NetworkAddress _v6address, Type type, unsigned char peerTag[16]) : address(_address), v6address(_v6address)
{
	this->id = id;
	this->port = port;
	this->type = type;
	memcpy(this->peerTag, peerTag, 16);
	if (type == Type::UDP_RELAY && ServerConfig::GetSharedInstance()->GetBoolean("force_tcp", false))
		this->type = Type::TCP_RELAY;

	lastPingSeq = 0;
	lastPingTime = 0;
	averageRTT = 0;
	socket = NULL;
	udpPongCount = 0;
}

Endpoint::Endpoint() : address(NetworkAddress::Empty()), v6address(NetworkAddress::Empty())
{
	lastPingSeq = 0;
	lastPingTime = 0;
	averageRTT = 0;
	socket = NULL;
	udpPongCount = 0;
}

const NetworkAddress &Endpoint::GetAddress() const
{
	return IsIPv6Only() ? (NetworkAddress &)v6address : (NetworkAddress &)address;
}

NetworkAddress &Endpoint::GetAddress()
{
	return IsIPv6Only() ? (NetworkAddress &)v6address : (NetworkAddress &)address;
}

bool Endpoint::IsIPv6Only() const
{
	return address.IsEmpty() && !v6address.IsEmpty();
}

int64_t Endpoint::CleanID() const
{
	int64_t _id = id;
	if (type == Type::TCP_RELAY)
	{
		_id = _id ^ ((int64_t)FOURCC('T', 'C', 'P', ' ') << 32);
	}
	if (IsIPv6Only())
	{
		_id = _id ^ ((int64_t)FOURCC('I', 'P', 'v', '6') << 32);
	}
	return _id;
}

Endpoint::~Endpoint()
{
	if (socket)
	{
		socket->Close();
	}
}
