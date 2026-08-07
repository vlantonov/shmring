#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include "shmring/ring_buffer.hpp"

// ── Unique SHM name helper ────────────────────────────────────────────────────

static std::string unique_shm_name(const char* tag) {
    static std::atomic<int> counter{0};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/%s_%d_%d",
                  tag, static_cast<int>(::getpid()), counter.fetch_add(1));
    return buf;
}

// ── BM_SpscThroughput ─────────────────────────────────────────────────────────
//
// Two threads in the same process: producer thread runs the benchmark loop
// (push); consumer thread busy-pops. Measures sustained push throughput.

static void BM_SpscThroughput(benchmark::State& state) {
    std::string name = unique_shm_name("bench_tput");
    using Ring = shmring::ShmRingBuffer<64, 4096>;
    Ring ring(name, true);

    std::atomic<bool> stop{false};

    std::thread consumer([&ring, &stop]() {
        char        buf[64] = {};
        std::size_t len     = 0;
        while (!stop.load(std::memory_order_relaxed))
            (void)ring.pop(buf, len);  // busy-drain
        while (ring.pop(buf, len)) {}  // drain remainder
    });

    char payload[64] = {};
    for (auto _ : state) {
        while (!ring.push(payload, 64))
            std::this_thread::yield();
    }

    stop.store(true, std::memory_order_relaxed);
    consumer.join();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscThroughput)->Iterations(1'000'000);

// ── BM_CrossProcessLatency ────────────────────────────────────────────────────
//
// Two ring buffers (ping + pong); a child process echoes: pop from ping,
// push to pong. Parent measures round-trip time per element.

static void BM_CrossProcessLatency(benchmark::State& state) {
    std::string ping_name = unique_shm_name("bench_ping");
    std::string pong_name = unique_shm_name("bench_pong");

    using Ring = shmring::ShmRingBuffer<64, 64>;
    Ring ping(ping_name, true);
    Ring pong(pong_name, true);

    pid_t child = ::fork();
    if (child == 0) {
        // Child: non-owner echo loop — pop from ping, push to pong.
        try {
            Ring c_ping(ping_name, false);
            Ring c_pong(pong_name, false);
            char        buf[64] = {};
            std::size_t len     = 0;
            while (true) {
                if (c_ping.pop(buf, len)) {
                    while (!c_pong.push(buf, len))
                        std::this_thread::yield();
                } else {
                    std::this_thread::yield();
                }
            }
        } catch (...) {}
        _exit(0);
    }

    char        payload[64] = {};
    char        reply[64]   = {};
    std::size_t reply_len   = 0;

    for (auto _ : state) {
        auto t0 = std::chrono::high_resolution_clock::now();

        while (!ping.push(payload, 64)) {}
        while (!pong.pop(reply, reply_len)) {}

        auto t1 = std::chrono::high_resolution_clock::now();
        state.SetIterationTime(
            std::chrono::duration<double>(t1 - t0).count());
    }

    ::kill(child, SIGTERM);
    ::waitpid(child, nullptr, 0);
    // ping and pong owners' destructors call shm_unlink
}
BENCHMARK(BM_CrossProcessLatency)->Iterations(10'000)->UseManualTime();
