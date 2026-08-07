# Design Document — shmring

**Version:** 0.1.0
**Date:** 2026-08-08
**Status:** Draft
**Derived from:** `docs/requirements/SRS.md` v1.0.0

---

## 0. Scope of This Document

This document translates the SRS into a concrete implementation blueprint. All decisions are bounded by the SRS. Where more than one reasonable design option exists, the trade-off is documented inline. Missing or ambiguous requirements are flagged as **⚠ Flag to Requirements Analyst**.

---

## 1. Architecture Overview

### 1.1 Component Diagram

```mermaid
graph TD
    subgraph lib [shmring — header-only library]
        ShmRegion["ShmRegion\nshm_region.hpp\nRAII shm_open / mmap"]
        RingBuffer["ShmRingBuffer&lt;ElementSize, Capacity&gt;\nring_buffer.hpp\nLock-free SPSC"]
        RingBuffer -->|composes| ShmRegion
    end

    subgraph demo [shmring_demo]
        Producer["shmring_demo_producer\nOwner = true\nlibpcap reader"]
        Consumer["shmring_demo_consumer\nOwner = false\npacket counter"]
    end

    subgraph tests [shmring_tests — Catch2]
        SP["Single-process\ncorrectness tests"]
        MP["Multi-process\nfork() test"]
    end

    subgraph bench [shmring_bench — Google Benchmark]
        BM1["BM_SpscThroughput"]
        BM2["BM_CrossProcessLatency"]
    end

    Producer -->|push()| RingBuffer
    Consumer -->|pop()| RingBuffer
    SP --> RingBuffer
    MP --> RingBuffer
    BM1 --> RingBuffer
    BM2 --> RingBuffer
```

### 1.2 Module Summary

| Module | File(s) | Description |
|--------|---------|-------------|
| `ShmRegion` | `include/shmring/shm_region.hpp` | RAII wrapper for `shm_open` / `mmap` lifecycle |
| `ShmRingBuffer` | `include/shmring/ring_buffer.hpp` | Header-only SPSC ring buffer template |
| Demo producer | `src/demo/producer.cpp` | libpcap reader → ring buffer |
| Demo consumer | `src/demo/consumer.cpp` | ring buffer → packet stats |
| Tests | `tests/test_ring_buffer.cpp` | Catch2 correctness suite |
| Benchmarks | `benchmarks/bench_ring_buffer.cpp` | Google Benchmark suite |

---

## 2. Repository Layout

```
shmring/
  CMakeLists.txt          # root; Conan 2 for Catch2 and Google Benchmark
  VERSION                 # 0.1.0
  include/shmring/
    ring_buffer.hpp       # ShmRingBuffer<ElementSize, Capacity> — header-only
    shm_region.hpp        # RAII shm_open/mmap wrapper — header-only
  src/
    demo/
      producer.cpp
      consumer.cpp
      CMakeLists.txt
  tests/
    test_ring_buffer.cpp  # Catch2 tests
    CMakeLists.txt
  benchmarks/
    bench_ring_buffer.cpp # Google Benchmark
    CMakeLists.txt
```

---

## 3. Shared-Memory Layout

### 3.1 In-SHM Struct Definitions

All structs are POD / standard-layout. No `std::` types, no vtables, no pointers reside in the mapped region.

```cpp
// ── ShmHeader: 64 bytes, one cache line ─────────────────────────────────────
struct ShmHeader {
    uint32_t magic;        // 0x534D5246 = "SMRF"
    uint32_t version;      // layout version; currently 1
    uint32_t element_size; // max payload bytes per slot (== ElementSize template param)
    uint32_t capacity;     // number of slots (== Capacity template param)
    uint32_t flags;        // bit 0 = PRODUCER_DONE; access via std::atomic_ref<uint32_t>
    uint8_t  _pad[44];     // pad struct to exactly 64 bytes
};
static_assert(sizeof(ShmHeader) == 64);

// ── Cache-line-padded atomic index ───────────────────────────────────────────
struct alignas(64) CacheLinePadded64 {
    std::atomic<uint64_t> value{0};
    uint8_t _pad[64 - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(CacheLinePadded64)  == 64);
static_assert(alignof(CacheLinePadded64) == 64);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
```

### 3.2 Flat SHM Memory Map

```
Offset   0 : ShmHeader           (64 bytes)
Offset  64 : head                CacheLinePadded64  (64 bytes) — producer writes
Offset 128 : tail                CacheLinePadded64  (64 bytes) — consumer writes
Offset 192 : data[]              (sizeof(uint32_t) + ElementSize) × Capacity bytes
             Each slot = [ uint32_t stored_len | uint8_t payload[ElementSize] ]
```

**Total SHM size** = `192 + (4 + ElementSize) × Capacity` bytes.

### 3.3 Field Table

| Field | Offset (bytes) | Size (bytes) | Notes |
|-------|---------------|--------------|-------|
| `magic` | 0 | 4 | `0x534D5246` |
| `version` | 4 | 4 | `1` |
| `element_size` | 8 | 4 | compile-time `ElementSize` |
| `capacity` | 12 | 4 | compile-time `Capacity` |
| `flags` | 16 | 4 | bit 0 = PRODUCER_DONE |
| `_pad` | 20 | 44 | zero |
| `head.value` | 64 | 8 | next write index (uint64_t) |
| `head._pad` | 72 | 56 | zero |
| `tail.value` | 128 | 8 | next read index (uint64_t) |
| `tail._pad` | 136 | 56 | zero |
| `data[0]` | 192 | 4 + ElementSize | first slot |
| `data[i]` | 192 + i×(4+ElementSize) | 4 + ElementSize | slot `i` |

### 3.4 Slot Layout

Each slot at index `i = head_or_tail & (Capacity - 1)`:

```
bytes [0 .. 3]               : uint32_t stored_len  — bytes written by push()
bytes [4 .. 3 + ElementSize] : uint8_t  payload     — first stored_len bytes are valid
```

### 3.5 Design Note: `std::atomic_ref` for `flags`

`ShmHeader::flags` is a plain `uint32_t` (POD). All cross-process accesses go through `std::atomic_ref<uint32_t>` (C++20), which provides atomic semantics without embedding a `std::atomic` in the struct. This preserves ABI portability of the struct (FR-01.3 / NFR-03) while meeting HC-02.

---

## 4. Key Interfaces

### 4.1 `ShmRegion` (include/shmring/shm_region.hpp)

```cpp
class ShmRegion {
public:
    // Creates (owner=true) or attaches to (owner=false) a POSIX SHM segment.
    // Throws std::system_error with errno on shm_open / ftruncate / mmap failure.
    ShmRegion(std::string_view name, std::size_t size, bool owner);

    ~ShmRegion() noexcept;  // munmap always; shm_unlink iff owner_ == true

    ShmRegion(const ShmRegion&)            = delete;
    ShmRegion& operator=(const ShmRegion&) = delete;
    ShmRegion(ShmRegion&&)                 noexcept;
    ShmRegion& operator=(ShmRegion&&)      noexcept;

    [[nodiscard]] void*       data()  noexcept;
    [[nodiscard]] const void* data()  const noexcept;
    [[nodiscard]] std::size_t size()  const noexcept;

private:
    void*        ptr_   = MAP_FAILED;
    std::size_t  size_  = 0;
    std::string  name_;
    bool         owner_ = false;
};
```

**Ownership model:** `ShmRegion` owns the mapping exclusively. Move-only. The destructor is the sole cleanup path (RAII; FR-04.1, FR-04.2, FR-04.3).

### 4.2 `ShmRingBuffer<ElementSize, Capacity>` (include/shmring/ring_buffer.hpp)

```cpp
template <std::size_t ElementSize, std::size_t Capacity>
    requires (ElementSize >= sizeof(uint32_t))
          && (Capacity > 0)
          && ((Capacity & (Capacity - 1)) == 0)  // power-of-two
class ShmRingBuffer {
public:
    // owner=true : creates SHM, initialises ShmHeader, zeroes head/tail.
    // owner=false: attaches to existing SHM, validates magic + version +
    //              element_size + capacity against template parameters.
    // Throws std::system_error on POSIX failure.
    // Throws std::runtime_error on header field mismatch (ABI guard).
    explicit ShmRingBuffer(std::string_view name, bool owner);

    ~ShmRingBuffer() noexcept;  // delegates to ShmRegion

    ShmRingBuffer(const ShmRingBuffer&)            = delete;
    ShmRingBuffer& operator=(const ShmRingBuffer&) = delete;

    // Copies len bytes (len <= ElementSize) into the next available slot.
    // Returns false immediately if the buffer is full.
    // Behaviour is undefined if len > ElementSize.
    [[nodiscard]] bool push(const void* data, std::size_t len) noexcept;

    // Copies the oldest slot's payload into out; writes stored length into out_len.
    // Returns false immediately if the buffer is empty.
    [[nodiscard]] bool pop(void* out, std::size_t& out_len) noexcept;

    // Total bytes required for the SHM segment (used internally and by tests).
    static constexpr std::size_t required_shm_size() noexcept;

private:
    ShmRegion          region_;
    ShmHeader*         header_ = nullptr;  // points into SHM at offset 0
    CacheLinePadded64* head_   = nullptr;  // points into SHM at offset 64
    CacheLinePadded64* tail_   = nullptr;  // points into SHM at offset 128
    std::byte*         data_   = nullptr;  // points into SHM at offset 192

    static constexpr std::size_t kSlotSize = sizeof(uint32_t) + ElementSize;
    static constexpr std::size_t kMask     = Capacity - 1;
    static constexpr uint32_t    kMagic    = 0x534D5246u;  // "SMRF"
    static constexpr uint32_t    kVersion  = 1u;
    static constexpr uint32_t    kFlagDone = 0x1u;
};
```

---

## 5. Memory-Ordering Rationale

### 5.1 Conceptual Role of Each Atomic

```
Producer process                   Consumer process
─────────────────                  ─────────────────
Sole writer of: head               Sole writer of: tail
Sole reader of: (own head)         Sole reader of: (own tail)
Cross-reads:    tail (to check     Cross-reads:    head (to check
                buffer not full)                   buffer not empty)
Writes data:    slot[head & mask]  Reads data:     slot[tail & mask]
```

Because each atomic is written by exactly one side (SPSC invariant), the writer can use `relaxed` for its own self-read and the cross-reader must use `acquire` to synchronise with the writer's `release` store.

### 5.2 `push()` — Memory Ordering at Each Atomic Call

```cpp
bool push(const void* src, std::size_t len) noexcept {
    uint64_t h = head_->value.load(std::memory_order_relaxed);
    // relaxed: producer owns head_; reading our own counter needs no cross-thread fence

    uint64_t t = tail_->value.load(std::memory_order_acquire);
    // acquire: pairs with consumer's tail_.store(release) in pop();
    //   ensures the producer does not observe the slot as free until the consumer
    //   has completed all reads from it — i.e., the consumer's memcpy happens-before
    //   this acquire load, so the producer can safely overwrite the slot

    if (h - t >= Capacity) return false;   // buffer full; non-blocking

    std::byte* slot = data_ + (h & kMask) * kSlotSize;
    auto len32 = static_cast<uint32_t>(len);
    std::memcpy(slot,                    &len32, sizeof(uint32_t));
    std::memcpy(slot + sizeof(uint32_t), src,   len);
    // plain writes; the release store below ensures they are visible to the consumer

    head_->value.store(h + 1, std::memory_order_release);
    // release: publishes the completed slot writes to any thread/process that
    //   subsequently loads head_ with acquire — the consumer's head_.load(acquire)
    //   in pop() will see both the new index and the fully-written slot data

    return true;
}
```

### 5.3 `pop()` — Memory Ordering at Each Atomic Call

```cpp
bool pop(void* dst, std::size_t& out_len) noexcept {
    uint64_t t = tail_->value.load(std::memory_order_relaxed);
    // relaxed: consumer owns tail_; reading our own counter needs no cross-thread fence

    uint64_t h = head_->value.load(std::memory_order_acquire);
    // acquire: pairs with producer's head_.store(release) in push();
    //   establishes happens-before so every memcpy the producer performed before its
    //   release store is visible here — without this acquire, the consumer could
    //   observe a new head value but stale slot bytes on weakly-ordered CPUs

    if (h == t) return false;              // buffer empty; non-blocking

    const std::byte* slot = data_ + (t & kMask) * kSlotSize;
    uint32_t stored_len;
    std::memcpy(&stored_len,                slot,                    sizeof(uint32_t));
    std::memcpy(dst,                        slot + sizeof(uint32_t), stored_len);
    // reads happen-before the release store below

    out_len = stored_len;

    tail_->value.store(t + 1, std::memory_order_release);
    // release: signals to the producer that the slot is free to reuse;
    //   pairs with the producer's tail_.load(acquire) in push() — without this
    //   release, the producer could begin overwriting the slot while the consumer's
    //   memcpy is still in-flight on a weakly-ordered CPU (AArch64, RISC-V)

    return true;
}
```

### 5.4 Summary Table

| Call site | Atomic variable | Memory order | One-line justification |
|-----------|----------------|-------------|------------------------|
| `push()` — load `head` | `head_` | `relaxed` | Producer owns `head_`; self-read, no cross-process synchronisation needed |
| `push()` — load `tail` | `tail_` | `acquire` | Pairs with `pop()`'s `tail_.store(release)`; ensures slot is fully read before producer reuses it |
| `push()` — store `head` | `head_` | `release` | Publishes completed slot writes; consumer's `head_.load(acquire)` in `pop()` sees the data |
| `pop()` — load `tail` | `tail_` | `relaxed` | Consumer owns `tail_`; self-read, no cross-process synchronisation needed |
| `pop()` — load `head` | `head_` | `acquire` | Pairs with `push()`'s `head_.store(release)`; ensures fully-written slot bytes are visible |
| `pop()` — store `tail` | `tail_` | `release` | Signals slot is free; producer's `tail_.load(acquire)` in `push()` sees the freed slot |

### 5.5 TSO Note

On x86-64 (Total Store Order), every store is implicitly release-ordered at the hardware level, so the `release` annotations compile to plain `MOV` instructions. `acquire` loads similarly compile to plain `MOV`. The explicit annotations are **required** by the C++ abstract machine to prevent compiler reordering and to produce correct code on weakly-ordered ISAs (AArch64, RISC-V) where the acquire/release pairs generate the necessary load-acquire and store-release instructions.

---

## 6. Demo Architecture

### 6.1 Executables and Parameters

```
shmring_demo_producer  <pcap_file>   # Owner = true
shmring_demo_consumer                # Owner = false
```

**Compile-time constants for the demo:**
- `ElementSize = 1500` (Ethernet MTU, bytes)
- `Capacity    = 4096` (power-of-two; SHM data region ≈ 6 MB)

### 6.2 Startup Protocol

```
┌─────────────────────┐       /dev/shm/shmring_demo        ┌─────────────────────┐
│     Producer        │                                     │     Consumer        │
│                     │  1. shm_open(O_CREAT|O_RDWR)       │                     │
│  create SHM         │──────────────────────────────────>  │                     │
│  ftruncate          │                                     │  2. retry loop:     │
│  mmap               │                                     │  shm_open(O_RDWR)   │
│  write ShmHeader    │                                     │  every 100 ms up    │
│  (magic, version,   │                                     │  to 5 s timeout     │
│   element_size,     │                                     │                     │
│   capacity,flags=0) │  3. magic visible (atomic_ref) ──> │  3. spin on magic   │
│                     │                                     │  until == 0x534D5246│
│  4. open pcap       │                                     │  (acquire, 1 s max) │
│  push() packets ────│──── SHM data ───────────────────── │──> pop() packets    │
│                     │                                     │                     │
│  5. pcap EOF:       │                                     │                     │
│  set flags bit 0    │  6. flags visible (atomic_ref) ──> │  drain buffer;      │
│  (release)          │                                     │  print summary;     │
│  exit 0             │                                     │  exit 0             │
└─────────────────────┘                                     └─────────────────────┘
```

**Partial startup handling:**
- The consumer polls `shm_open()` (no `O_CREAT`) at 100 ms intervals for up to 5 seconds.
- After `shm_open` succeeds, the consumer spins on `std::atomic_ref<uint32_t>(header->magic).load(acquire)` for up to 1 second before validating the remaining header fields.
- A mismatch in `element_size` or `capacity` causes the consumer to print an error and exit with a non-zero code (ABI guard).

### 6.3 Shutdown Protocol

| Event | Producer action | Consumer action |
|-------|----------------|----------------|
| pcap EOF | Spin-retry `push()` for remaining packets, then `flags \|= PRODUCER_DONE` (release), `exit(0)` | Drain buffer; when empty and `PRODUCER_DONE` set, print summary, `exit(0)` |
| SIGTERM | Signal handler sets `std::sig_atomic_t done`; main loop exits after current push attempt | Signal handler sets `std::sig_atomic_t done`; main loop exits after current pop attempt |
| Consumer crash | Producer continues; buffer fills; `push()` returns false; spin-retry until SIGTERM | — |
| Producer crash | SHM persists; consumer times out after configurable idle period (default 5 s), prints partial summary, `exit(0)` | — |

### 6.4 Producer Pseudocode

```
open_pcap(argv[1])
ring = ShmRingBuffer<1500, 4096>("shmring_demo", owner=true)
while pcap_next_ex() != EOF and not sigterm_received:
    while not ring.push(packet, packet_len):     // spin outside hot path
        if sigterm_received: goto done
        std::this_thread::yield()
done:
atomic_ref(header->flags).store(flags | PRODUCER_DONE, release)
exit(0)
```

### 6.5 Consumer Pseudocode

```
wait for SHM (retry shm_open up to 5 s, sleep 100 ms between attempts)
ring = ShmRingBuffer<1500, 4096>("shmring_demo", owner=false)
validate header (magic, version, element_size, capacity)
packet_count = 0; total_bytes = 0; idle_elapsed = 0
while not sigterm_received:
    if ring.pop(buf, len):
        packet_count++; total_bytes += len; idle_elapsed = 0
        update first-byte histogram
    else:
        done = atomic_ref(header->flags).load(acquire) & PRODUCER_DONE
        if done and ring is empty: break
        sleep(sleep_quantum); idle_elapsed += sleep_quantum
        if idle_elapsed > idle_timeout: break
print(packet_count, total_bytes, histogram)
exit(0)
```

---

## 7. CMake Target Structure

### 7.1 Root CMakeLists.txt Responsibilities

```cmake
cmake_minimum_required(VERSION 3.20)
project(shmring VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic)

# ── Conan 2: Catch2 v3 and Google Benchmark ──────────────────────────────────
find_package(Catch2 REQUIRED)
find_package(benchmark REQUIRED)

# ── Header-only interface library ────────────────────────────────────────────
add_library(shmring_headers INTERFACE)
target_include_directories(shmring_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(shmring_headers INTERFACE cxx_std_20)

# ── Subdirectories ────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(src/demo)
add_subdirectory(tests)
add_subdirectory(benchmarks)
```

### 7.2 Target Dependency Graph

```
shmring_headers (INTERFACE, header-only)
    ├── shmring_demo_producer  PRIVATE: shmring_headers, ${PCAP_LIBRARIES}, rt
    ├── shmring_demo_consumer  PRIVATE: shmring_headers, rt
    ├── shmring_tests          PRIVATE: shmring_headers, Catch2::Catch2WithMain, rt
    └── shmring_bench          PRIVATE: shmring_headers, benchmark::benchmark_main, rt
```

`rt` provides `shm_open` / `shm_unlink` on Linux. Link it `PRIVATE` to each consuming target; it is an implementation detail, not part of the public API.

### 7.3 src/demo/CMakeLists.txt

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(PCAP REQUIRED libpcap)

add_executable(shmring_demo_producer producer.cpp)
target_link_libraries(shmring_demo_producer PRIVATE
    shmring_headers ${PCAP_LIBRARIES} rt)
target_include_directories(shmring_demo_producer PRIVATE
    ${PCAP_INCLUDE_DIRS})

add_executable(shmring_demo_consumer consumer.cpp)
target_link_libraries(shmring_demo_consumer PRIVATE
    shmring_headers rt)
```

### 7.4 tests/CMakeLists.txt

```cmake
add_executable(shmring_tests test_ring_buffer.cpp)
target_link_libraries(shmring_tests PRIVATE
    shmring_headers Catch2::Catch2WithMain rt)

include(Catch)
catch_discover_tests(shmring_tests)  # auto-registers all TEST_CASE blocks with CTest
```

### 7.5 benchmarks/CMakeLists.txt

```cmake
add_executable(shmring_bench bench_ring_buffer.cpp)
target_link_libraries(shmring_bench PRIVATE
    shmring_headers benchmark::benchmark_main rt)
# Intentionally not added to CTest; benchmarks are run manually.
```

---

## 8. Test Strategy

### 8.1 Single-Process Tests (FR-06.1)

| Test name | FR / NFR | What it exercises |
|-----------|---------|------------------|
| `PushTrueWhenSlotAvailable` | FR-02.1 | `push()` to empty buffer returns `true` |
| `PushFalseWhenFull` | FR-02.2 | exactly `Capacity` pushes succeed; `Capacity+1`-th returns `false` |
| `PopFalseWhenEmpty` | FR-02.4 | `pop()` on empty buffer returns `false` |
| `PopTrueWhenDataAvailable` | FR-02.3 | push followed by pop returns `true` |
| `DataIntegrityRoundTrip` | FR-02.3 | bytes pushed equal bytes popped (`std::memcmp`) |
| `LengthFieldRoundTrip` | FR-02.3 | `out_len` from `pop()` equals `len` passed to `push()` |
| `CapacityBoundaryEmpty` | FR-02.4 | `Capacity` push/pop pairs leave the buffer empty; next `pop()` returns `false` |
| `ShmHeaderMagicValid` | NFR-03 | owner construction writes `magic == 0x534D5246` |
| `ShmHeaderVersionValid` | NFR-03 | owner construction writes `version == 1` |
| `ShmHeaderFieldsMatchTemplate` | NFR-03 | `element_size == ElementSize`, `capacity == Capacity` |
| `CacheLineAlignmentHead` | NFR-02 | `offsetof(head_.value)` from base of mapped region is `64` |
| `CacheLineAlignmentTail` | NFR-02 | `offsetof(tail_.value)` from base is `128` |
| `HeadTailSeparateCacheLines` | NFR-02 | `&head_` and `&tail_` differ by ≥ 64 bytes |
| `OwnerDestructorUnlinks` | FR-04.2 | after owner dtor, `shm_open(O_RDWR)` fails with `ENOENT` |
| `NonOwnerDestructorNoUnlink` | FR-04.2 | after non-owner dtor, SHM segment is still accessible |
| `ConstructionThrowsOnPosixError` | FR-03.4 | passing an invalid SHM name throws `std::system_error` containing `errno` |
| `NonOwnerThrowsIfSegmentAbsent` | FR-03.3 | `owner=false` on a non-existent segment throws `std::system_error` |
| `ShmLayoutStaticAsserts` | NFR-03 | compile-time `static_assert` on `sizeof(ShmHeader)`, `alignof(CacheLinePadded64)`, `is_always_lock_free` |

### 8.2 Multi-Process Test (FR-06.2)

| Test name | Mechanism | What it exercises |
|-----------|-----------|------------------|
| `MultiProcessSPSC` | `fork()` + `waitpid()` | End-to-end correctness across separate producer/consumer processes sharing one SHM segment |

**Implementation sketch:**

```
TEST_CASE("MultiProcessSPSC") {
    constexpr int N = 1024;
    // Parent creates the ring as owner.
    pid_t pid = fork();
    if (pid == 0) {
        // Child (non-owner): push N packets with known 8-byte payload pattern.
        // Set PRODUCER_DONE flag, exit(0).
    } else {
        // Parent (owner): pop until N packets received or 5 s timeout.
        // Verify packet count, payload content, and stored lengths.
        int status;
        waitpid(pid, &status, 0);
        REQUIRE(WIFEXITED(status));
        REQUIRE(WEXITSTATUS(status) == 0);
    }
    // Parent destructor calls shm_unlink.
}
```

### 8.3 CTest Integration (FR-06.3)

`catch_discover_tests(shmring_tests)` auto-registers all `TEST_CASE` blocks. Run with:

```bash
ctest --test-dir build --output-on-failure
```

---

## 9. Benchmark Design

### 9.1 `BM_SpscThroughput`

- **Purpose:** Measure sustained push+pop throughput in the single-process, same-thread case (establishes peak hardware ceiling with no scheduling overhead).
- **Setup:** Construct `ShmRingBuffer<64, 4096>` with a temp SHM name (unique per benchmark run via `mkstemp`-style naming).
- **Loop body:** `push(data, 64)` followed immediately by `pop(out, len)` in the same thread; repeated for `state.iterations()` iterations.
- **Metric:** `state.SetItemsProcessed(state.iterations())` → Google Benchmark reports `items/s`.
- **Variants:** parameterise on `Capacity` via `BENCHMARK(BM_SpscThroughput)->Arg(512)->Arg(4096)->Arg(65536)` to observe cache-size effects.
- **Teardown:** Owner destructor unlinks the SHM segment.

### 9.2 `BM_CrossProcessLatency`

- **Purpose:** Measure round-trip latency for a single element crossing a process boundary via two ring buffers (ping-pong).
- **Setup (RAII fixture):**
  - Parent creates two owner ring buffers: `ring_ping` and `ring_pong` (each `ShmRingBuffer<64, 4096>`).
  - Parent `fork()`s a child that:
    - Attaches to both as non-owner.
    - Runs a tight busy-spin loop: `pop(ring_ping)` → `push(ring_pong)`.
    - Exits when it receives SIGTERM.
- **Loop body (parent, using `UseManualTime()`):**
  ```
  t0 = chrono::high_resolution_clock::now()
  ring_ping.push(payload, 8)
  while not ring_pong.pop(out, len): /* busy spin */
  t1 = chrono::high_resolution_clock::now()
  state.SetIterationTime((t1 - t0).count() * 1e-9)
  ```
- **Metric:** Round-trip latency in nanoseconds (mean + p99 visible via `--benchmark_format=json --benchmark_repetitions=10`).
- **Teardown:** Send `SIGTERM` to child, call `waitpid()`, owner destructors unlink both SHM segments.

---

## 10. Build Instructions

```bash
# Prerequisites (Debian/Ubuntu)
sudo apt-get install -y cmake g++ libpcap-dev python3-pip
pip install conan

# Detect your toolchain (first-time only)
conan profile detect

# Install dependencies via Conan 2 (copies packages into build/ for portability)
conan install . --output-folder=build --build=missing --deployer=full_deploy

# Configure and build
cmake -S . -B build \
      -DCMAKE_TOOLCHAIN_FILE=build/build/Release/generators/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# Run benchmarks
./build/benchmarks/shmring_bench --benchmark_format=json
```

---

## 11. Design Decisions and Trade-offs

| # | Decision | Alternative considered | Reason chosen |
|---|----------|----------------------|---------------|
| D-01 | Header-only library template | Explicit `.cpp` instantiations | Avoids the need to enumerate all `(ElementSize, Capacity)` pairs at library build time; easier for users with custom params |
| D-02 | `std::atomic_ref<uint32_t>` for `ShmHeader::flags` | Embed `std::atomic<uint32_t>` in `ShmHeader` directly | Keeps `ShmHeader` as a pure POD struct (FR-01.3 / NFR-03); avoids `std::` internals in the mapped region |
| D-03 | `uint64_t` indices (no wraparound sentinel logic) | `uint32_t` with explicit modulo-wrap handling | 64-bit counters never overflow at realistic packet rates; eliminates a subtle correctness hazard without overhead |
| D-04 | Power-of-two `Capacity` enforced via C++20 `requires` | Runtime assertion | Bitwise AND `& (Capacity - 1)` instead of modulo on the hot path (NFR-01); mistake caught at compile time |
| D-05 | Per-slot `uint32_t` length prefix stored inline in the data array | Fixed-size copy always copying `ElementSize` bytes | Supports variable-length payloads per FR-02.3; pop() can faithfully return `out_len` without heap allocation |
| D-06 | `ShmRegion` as a separate RAII class | POSIX calls inlined in `ShmRingBuffer` | Separation of concerns; `ShmRegion` is independently testable and allows future reuse |
| D-07 | Producer-first startup; consumer retries `shm_open` | Rendezvous segment or named semaphore | Simpler for an SPSC demo; a rendezvous mechanism adds complexity not required by the SRS |
| D-08 | `PRODUCER_DONE` communicated via `flags` bit in `ShmHeader` | Sentinel packet with a magic payload | Clean separation of control plane from data plane; no element-size-specific sentinel value required |
| D-09 | Conan 2 for Catch2 and Google Benchmark; system package for libpcap | FetchContent for test/bench dependencies | Pinned, reproducible dependency resolution via ConanCenter; libpcap has no ConanCenter package suitable for all CI setups and is universally available as a system package |
| D-10 | `relaxed` on producer's self-load of `head` and consumer's self-load of `tail` | `acquire` on all atomic loads | Correct per C++ memory model: self-reads need no cross-process fence; reduces unnecessary barriers on weakly-ordered CPUs |

---

## 12. Risks and Flags to Requirements Analyst

| # | Risk / Missing Requirement | Severity | Mitigation / Flag |
|---|---------------------------|----------|-------------------|
| R-01 | Stale SHM persists if producer crashes without calling `shm_unlink` | Medium | Consumer validates magic + version on attach; document manual cleanup (`rm /dev/shm/shmring_demo`); add a `--cleanup` CLI flag in the demo |
| R-02 | `std::atomic<uint64_t>` may not be lock-free on 32-bit or exotic platforms | Low (x86-64 assumed per §7 of SRS) | `static_assert(std::atomic<uint64_t>::is_always_lock_free)` in `ring_buffer.hpp` |
| R-03 | Race: consumer attaches before producer finishes initialising the header | Low | Consumer spins on `magic` field (acquire load via `atomic_ref`) before reading other header fields (§6.2) |
| R-04 | ⚠ **SRS does not specify the idle timeout value** for the consumer (FR-05.3 says "N-second idle") | Medium | Design defaults to 5 s. SRS should either specify the value or define it as a CLI argument |
| R-05 | ⚠ **SRS does not specify `ElementSize` and `Capacity` defaults for the demo** | Low | Design uses 1500 and 4096 respectively. SRS should specify or allow CLI override |
| R-06 | libpcap absent on CI machines | Medium | Wrap demo target in a CMake `find_package(PkgConfig)` optional guard; skip building demo if libpcap not found, with a `message(STATUS ...)` |

---

*Design is ready for the C++ Developer agent to implement.*
