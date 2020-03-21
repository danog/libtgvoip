#include <cstdint>

// Protocol name/version
enum ProtocolVersions : int
{
    PROTOCOL_OLD = 9,
    PROTOCOL_RELIABLE = 10
};

#define PROTOCOL_NAME 0x50567247 // "GrVP" in little endian (reversed here)
#define PROTOCOL_VERSION 9
#define MIN_PROTOCOL_VERSION 3

namespace tgvoip
{
struct VersionInfo
{
    VersionInfo(int _peerVersion, int32_t _connectionMaxLayer) : peerVersion(_peerVersion), connectionMaxLayer(_connectionMaxLayer){};
    const int peerVersion = 0;
    const int32_t connectionMaxLayer = 0;

    inline bool isNew() const
    {
        return peerVersion >= PROTOCOL_RELIABLE || connectionMaxLayer >= 110;
    }
    inline bool isLegacyLegacy() const
    {
        return !(peerVersion >= 8 || (!peerVersion && connectionMaxLayer >= 92));
    }
};
} // namespace tgvoip