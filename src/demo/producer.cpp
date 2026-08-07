#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef SHMRING_HAVE_PCAP
#  include <pcap.h>
#endif

#include "shmring/ring_buffer.hpp"

static constexpr std::size_t kElementSize = 4096;
static constexpr std::size_t kCapacity    = 1024;
static constexpr const char* kShmName     = "/shmring_demo";

static volatile std::sig_atomic_t g_done = 0;

static void on_signal(int) noexcept { g_done = 1; }

int main(int argc, char* argv[]) {
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT,  on_signal);

    shmring::ShmRingBuffer<kElementSize, kCapacity> ring(kShmName, /*owner=*/true);

#ifdef SHMRING_HAVE_PCAP
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <pcap_file>\n", argv[0]);
        return 1;
    }
    char    errbuf[PCAP_ERRBUF_SIZE];
    pcap_t* handle = pcap_open_offline(argv[1], errbuf);
    if (!handle) {
        std::fprintf(stderr, "pcap_open_offline: %s\n", errbuf);
        return 1;
    }
    {
        struct pcap_pkthdr* hdr = nullptr;
        const u_char*       pkt = nullptr;
        int rc;
        while (!g_done && (rc = pcap_next_ex(handle, &hdr, &pkt)) == 1) {
            std::size_t len = std::min<std::size_t>(hdr->caplen, kElementSize);
            while (!ring.push(pkt, len) && !g_done)
                std::this_thread::yield();
        }
        pcap_close(handle);
    }
#else
    (void)argc;
    (void)argv;
    static constexpr int         kCount  = 10000;
    static constexpr std::size_t kPktLen = 64;
    {
        uint8_t buf[kPktLen];
        for (int i = 0; i < kCount && !g_done; ++i) {
            std::memset(buf, static_cast<uint8_t>(i & 0xFF), kPktLen);
            while (!ring.push(buf, kPktLen) && !g_done)
                std::this_thread::yield();
        }
    }
#endif

    // Signal consumer that no more data will be written.
    ring.header()->flags.store(shmring::ShmHeader::kFlagDone,
                               std::memory_order_release);
    return 0;
}
