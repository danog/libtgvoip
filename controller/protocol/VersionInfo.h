#pragma once
#include <cstdint>

// Protocol name/version
enum ProtocolVersions : int
{
    PROTOCOL_OLD = 9,
    PROTOCOL_RELIABLE = 10
};

#define PROTOCOL_NAME 0x50567247 // "GrVP" in little endian (reversed here)
#define PROTOCOL_VERSION 10
#define MIN_PROTOCOL_VERSION 3

namespace tgvoip
{
struct VersionInfo
{
    VersionInfo() = default;
    VersionInfo(int _peerVersion, int32_t _connectionMaxLayer) : peerVersion(_peerVersion), connectionMaxLayer(_connectionMaxLayer){};
    int peerVersion = 0;
    int32_t connectionMaxLayer = 0;

    uint32_t maxVideoResolution;

    inline bool isNew() const
    {
        return peerVersion >= PROTOCOL_RELIABLE || connectionMaxLayer >= 110;
    }
    inline bool isLegacy() const
    {
        return !isNew() && (peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92));
    }
    inline bool isLegacyLegacy() const
    {
        return !(peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92));
    }
};
} // namespace tgvoip