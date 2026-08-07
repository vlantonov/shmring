#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

#include "shmring/ring_buffer.hpp"

static constexpr std::size_t kElementSize   = 4096;
static constexpr std::size_t kCapacity      = 1024;
static constexpr const char* kShmName       = "/shmring_demo";
static constexpr int         kIdleTimeoutMs = 5000;
static constexpr int         kRetryMs       = 100;

static volatile std::sig_atomic_t g_done = 0;

static void on_signal(int) noexcept { g_done = 1; }

using Ring = shmring::ShmRingBuffer<kElementSize, kCapacity>;

int main() {
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT,  on_signal);

    // Retry attaching to the SHM until the producer creates it (up to 5 s).
    std::unique_ptr<Ring> ring;
    {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(kIdleTimeoutMs);
        while (!ring && std::chrono::steady_clock::now() < deadline && !g_done) {
            try {
                ring = std::make_unique<Ring>(kShmName, /*owner=*/false);
            } catch (const std::exception&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryMs));
            }
        }
    }
    if (!ring) {
        std::fprintf(stderr, "consumer: failed to attach to '%s' within %d ms\n",
                     kShmName, kIdleTimeoutMs);
        return 1;
    }

    uint8_t     buf[kElementSize];
    std::size_t len        = 0;
    uint64_t    pkt_count  = 0;
    uint64_t    total_bytes = 0;
    auto        last_recv  = std::chrono::steady_clock::now();

    while (!g_done) {
        if (ring->pop(buf, len)) {
            ++pkt_count;
            total_bytes += len;
            last_recv = std::chrono::steady_clock::now();
        } else {
            // Check PRODUCER_DONE flag with acquire to pair with producer's release store.
            bool done = (ring->header()->flags.load(std::memory_order_acquire)
                         & shmring::ShmHeader::kFlagDone) != 0;
            if (done) break;  // drain complete

            auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - last_recv)
                               .count();
            if (idle_ms >= kIdleTimeoutMs) {
                std::fprintf(stderr, "consumer: idle timeout (%d ms), exiting\n",
                             kIdleTimeoutMs);
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Drain any remaining items after PRODUCER_DONE.
    while (ring->pop(buf, len)) {
        ++pkt_count;
        total_bytes += len;
    }

    std::printf("packets=%" PRIu64 " bytes=%" PRIu64 "\n", pkt_count, total_bytes);
    return 0;
}
