#include <atomic>
#include <array>

namespace tgvoip
{
    struct Ack
    {
        // Ack specified ID + up to 32 seqs ago, specified by mask
        void ack(uint32_t ackId, uint32_t mask);
        // Check if seq was acked
        bool wasAcked(uint32_t seq);

        // Stream-specific seqno
        std::atomic<uint32_t> seq = ATOMIC_VAR_INIT(1);

        // Status list of acked seqnos, starting from the seq explicitly present in the packet + up to 32 seqs ago
        std::array<uint32_t, 33> peerAcks{0};
    };
}