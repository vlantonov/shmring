#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include "shmring/ring_buffer.hpp"
#include "shmring/shm_region.hpp"

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Generates a unique SHM name per test invocation using PID + monotonic counter.
static std::string make_shm_name() {
    static std::atomic<int> counter{0};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/shmring_test_%d_%d",
                  static_cast<int>(::getpid()), counter.fetch_add(1));
    return buf;
}

// ── Type aliases used across tests ───────────────────────────────────────────

using Ring8   = shmring::ShmRingBuffer<64, 8>;    // small capacity for boundary tests
using Ring128 = shmring::ShmRingBuffer<64, 128>;  // room for 100-element sequential test
using Ring1k  = shmring::ShmRingBuffer<64, 1024>; // thread-pair and multi-process tests

// ── Single-process correctness tests ─────────────────────────────────────────

TEST_CASE("push and pop single element") {
    std::string name = make_shm_name();
    Ring8 ring(name, true);

    char in_buf[64];
    std::memset(in_buf, 0xAB, sizeof(in_buf));

    REQUIRE(ring.push(in_buf, sizeof(in_buf)));

    char       out_buf[64] = {};
    std::size_t out_len    = 0;
    REQUIRE(ring.pop(out_buf, out_len));
    REQUIRE(out_len == sizeof(in_buf));
    REQUIRE(std::memcmp(in_buf, out_buf, sizeof(in_buf)) == 0);
}

TEST_CASE("push fills buffer") {
    std::string name = make_shm_name();
    Ring8 ring(name, true);  // Capacity = 8

    char buf[64] = {};
    for (std::size_t i = 0; i < 8; ++i)
        REQUIRE(ring.push(buf, 64));

    // One extra push must fail: buffer is full
    REQUIRE_FALSE(ring.push(buf, 64));
}

TEST_CASE("pop on empty returns false") {
    std::string name = make_shm_name();
    Ring8 ring(name, true);

    char        buf[64] = {};
    std::size_t len     = 0;
    REQUIRE_FALSE(ring.pop(buf, len));
}

TEST_CASE("sequential push/pop N elements") {
    std::string name = make_shm_name();
    Ring128 ring(name, true);  // Capacity=128 >= 100

    constexpr int kN = 100;

    // Push N elements with a distinct fill byte per index.
    for (int i = 0; i < kN; ++i) {
        char buf[64];
        std::memset(buf, i & 0xFF, sizeof(buf));
        REQUIRE(ring.push(buf, sizeof(buf)));
    }

    // Pop all N elements and verify byte patterns and lengths.
    for (int i = 0; i < kN; ++i) {
        char        out[64] = {};
        std::size_t len     = 0;
        REQUIRE(ring.pop(out, len));
        REQUIRE(len == 64u);
        for (int j = 0; j < 64; ++j)
            REQUIRE(static_cast<unsigned char>(out[j]) == static_cast<unsigned char>(i & 0xFF));
    }

    // Buffer must be empty now.
    char        out[64] = {};
    std::size_t len     = 0;
    REQUIRE_FALSE(ring.pop(out, len));
}

TEST_CASE("single-process producer-consumer thread pair") {
    std::string name = make_shm_name();
    Ring1k ring(name, true);

    constexpr int kN = 100000;
    std::atomic<bool> data_ok{true};
    std::atomic<int>  consumed{0};

    std::thread producer([&]() {
        char buf[64] = {};
        for (int i = 0; i < kN; ++i) {
            std::memcpy(buf, &i, sizeof(int));
            while (!ring.push(buf, 64))
                std::this_thread::yield();
        }
    });

    std::thread consumer([&]() {
        char        buf[64] = {};
        std::size_t len     = 0;
        int         expected = 0;
        while (expected < kN) {
            if (ring.pop(buf, len)) {
                int val = 0;
                std::memcpy(&val, buf, sizeof(int));
                if (val != expected || len != 64u)
                    data_ok.store(false, std::memory_order_relaxed);
                ++expected;
            }
        }
        consumed.store(expected, std::memory_order_relaxed);
    });

    producer.join();
    consumer.join();

    REQUIRE(data_ok.load());
    REQUIRE(consumed.load() == kN);
}

TEST_CASE("ShmRegion owner creates and non-owner attaches") {
    std::string name = make_shm_name();
    constexpr std::size_t kSize    = 4096;
    constexpr uint64_t    kPattern = 0xDEADBEEFCAFEBABEull;

    // Parent creates the region and writes a known pattern.
    shmring::ShmRegion owner_region(name, kSize, true);
    *static_cast<uint64_t*>(owner_region.data()) = kPattern;

    pid_t pid = ::fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        // Child: open as non-owner and verify the pattern.
        try {
            shmring::ShmRegion child_region(name, kSize, false);
            uint64_t val = *static_cast<const uint64_t*>(child_region.data());
            _exit(val == kPattern ? 0 : 1);
        } catch (...) {
            _exit(2);
        }
    }

    // Parent waits for the child and checks its exit code.
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited == -1 && errno == EINTR);
    REQUIRE(waited == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    // Owner destructor will call shm_unlink when owner_region goes out of scope.
}

// ── Layout / static-assert tests ─────────────────────────────────────────────

TEST_CASE("SHM layout static asserts") {
    // These are compile-time checks but wrapping in a TEST_CASE makes them
    // visible in the CTest output.
    static_assert(sizeof(shmring::ShmHeader) == 64);
    static_assert(sizeof(shmring::CacheLinePadded64)  == 64);
    static_assert(alignof(shmring::CacheLinePadded64) == 64);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    SUCCEED("static asserts passed");
}

TEST_CASE("ShmHeader fields initialised correctly by owner") {
    std::string name = make_shm_name();
    Ring8 ring(name, true);

    auto* h = ring.header();
    // acquire load pairs with owner constructor's release store of magic
    REQUIRE(std::atomic_ref<uint32_t>(h->magic).load(std::memory_order_acquire)
            == shmring::ShmHeader::kMagic);
    REQUIRE(h->version      == shmring::ShmHeader::kVersion);
    REQUIRE(h->element_size == 64u);
    REQUIRE(h->capacity     == 8u);
    REQUIRE(h->flags.load(std::memory_order_relaxed) == 0u);
}

TEST_CASE("head and tail occupy separate cache lines") {
    std::string name = make_shm_name();
    Ring8 ring(name, true);

    const auto* base = static_cast<const std::byte*>(
        static_cast<const void*>(ring.header()));

    // head is at offset 64, tail at offset 128 from the SHM base.
    const auto* head_ptr = base + sizeof(shmring::ShmHeader);
    const auto* tail_ptr = base + sizeof(shmring::ShmHeader)
                                + sizeof(shmring::CacheLinePadded64);

    std::ptrdiff_t sep = tail_ptr - head_ptr;
    REQUIRE(sep >= 64);

    // Both must be 64-byte aligned.
    REQUIRE(reinterpret_cast<uintptr_t>(head_ptr) % 64 == 0);
    REQUIRE(reinterpret_cast<uintptr_t>(tail_ptr) % 64 == 0);
}

TEST_CASE("owner destructor unlinks the SHM segment") {
    std::string name = make_shm_name();
    {
        Ring8 ring(name, true);
        // ring goes out of scope here → destructor calls shm_unlink
    }
    // shm_open without O_CREAT should now fail with ENOENT
    int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    REQUIRE(fd == -1);
    REQUIRE(errno == ENOENT);
}

TEST_CASE("non-owner destructor does not unlink the SHM segment") {
    std::string name = make_shm_name();
    Ring8 owner(name, true);
    {
        Ring8 nonowner(name, false);
        // nonowner goes out of scope → only munmap, no shm_unlink
    }
    // Segment must still be accessible.
    int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    REQUIRE(fd != -1);
    ::close(fd);
    // owner destructor will unlink when it goes out of scope
}

TEST_CASE("non-owner throws when segment is absent") {
    std::string name = make_shm_name();
    // No owner created — open must throw std::system_error (ENOENT).
    REQUIRE_THROWS_AS(Ring8(name, false), std::system_error);
}
