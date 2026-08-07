# Software Requirements Specification — shmring

**Version:** 1.0.0  
**Date:** 2026-08-08  
**Status:** Draft  

---

## 1. Purpose

This document specifies the requirements for **shmring**, a C++20 portfolio library that implements a single-producer / single-consumer (SPSC) lock-free ring buffer backed by POSIX shared memory (`shm_open` / `mmap`), together with a companion demo application that pipes raw pcap packets between two separate OS processes through that ring buffer.

The primary goals are:

- Demonstrate mastery of the C++ memory model (acquire/release/seq_cst orderings) without relying on mutex-based synchronisation in the hot path.
- Demonstrate low-latency inter-process communication (IPC) using POSIX shared memory on Linux.
- Provide a reproducible, benchmark-driven baseline for SPSC ring buffer performance.

---

## 2. Scope

### 2.1 In Scope

| Component | Description |
|-----------|-------------|
| `ShmRingBuffer<N, Cap>` | Class template forming the core library |
| `shmring_demo` | Producer/consumer demo binary using libpcap |
| Catch2 test suite | Correctness tests (single-process and multi-process) |
| Google Benchmark suite | Throughput and cross-process latency benchmarks |
| CMake build system | Full build, test, and benchmark targets |

### 2.2 Out of Scope (this iteration)

- Multi-producer / multi-consumer (MPMC) variants.
- Windows or macOS support (Linux-only for this version).
- Persistent replay or journaling of ring buffer contents.
- Network transport (the demo is local IPC only; no sockets).
- Dynamic resizing of the ring buffer after construction.
- Any GUI or monitoring dashboard.

---

## 3. Definitions and Abbreviations

| Term | Definition |
|------|-----------|
| **SPSC** | Single-Producer / Single-Consumer — exactly one writer thread/process and one reader thread/process |
| **SHM** | POSIX Shared Memory (`shm_open`, `mmap`) |
| **Hot path** | The `push()` / `pop()` call sequence executed per element |
| **Owner flag** | A boolean passed at construction indicating whether this instance creates the SHM segment (true) or merely attaches to one created by another process (false) |
| **Cache-line** | 64-byte aligned block of memory; padding prevents false sharing between the head and tail atomics |
| **pcap** | Packet capture file format (`.pcap`); raw packets read via `libpcap` |
| **FR** | Functional Requirement |
| **NFR** | Non-Functional Requirement |
| **HC** | Hard Constraint |

---

## 4. Functional Requirements

### FR-01 — `ShmRingBuffer` Class Template

**FR-01.1** The library shall provide a class template `ShmRingBuffer` parameterised on:
- `ElementSize` — the fixed size in bytes of each slot (`std::size_t`, compile-time constant).
- `Capacity` — the maximum number of slots (`std::size_t`, compile-time constant, must be a power of two).

**FR-01.2** The entire ring buffer state (head index, tail index, slot storage) shall reside within the mapped shared-memory region. No heap-allocated members (raw pointers, `std::vector`, `std::string`, etc.) shall be stored in the in-SHM layout.

**FR-01.3** The in-SHM layout shall contain no vtable pointers, no `std::` internal bookkeeping, and no compiler-version-dependent metadata, so that a producer binary and a consumer binary compiled independently can share the same segment without ABI mismatch.

---

### FR-02 — `push()` and `pop()` Operations

**FR-02.1** The producer interface shall expose a non-blocking `push(const void* src, std::size_t len) -> bool` (or equivalent) that copies `len ≤ ElementSize` bytes from `src` into the next available slot and advances the tail.

**FR-02.2** `push()` shall return `false` immediately (without blocking or spinning) when the buffer is full.

**FR-02.3** The consumer interface shall expose a non-blocking `pop(void* dst, std::size_t& len) -> bool` that copies the oldest slot into `dst`, writes the stored length into `len`, and advances the head.

**FR-02.4** `pop()` shall return `false` immediately when the buffer is empty.

**FR-02.5** Every load and store of the head and tail atomic indices shall carry an explicit `std::memory_order` argument. The rationale (acquire, release, or seq_cst) shall be documented in a single-line comment at each call site.

---

### FR-03 — Construction and SHM Lifecycle

**FR-03.1** The constructor shall accept:
- A POSIX SHM name (e.g., `/shmring_demo`).
- A total size (bytes) or shall compute it automatically from `ElementSize` and `Capacity`.
- An `Owner` boolean flag.

**FR-03.2** When `Owner == true`, the constructor shall call `shm_open` with `O_CREAT | O_RDWR`, `ftruncate` the segment to the required size, and `mmap` it.

**FR-03.3** When `Owner == false`, the constructor shall call `shm_open` with `O_RDWR` (no `O_CREAT`) and `mmap` the existing segment.

**FR-03.4** Construction shall throw a well-typed exception (or return an error via a factory) if `shm_open`, `ftruncate`, or `mmap` fails, providing the `errno` value in the error message.

---

### FR-04 — Destruction and Cleanup

**FR-04.1** The destructor shall call `munmap` on the mapped region.

**FR-04.2** When the instance was constructed with `Owner == true`, the destructor shall additionally call `shm_unlink` to remove the SHM object.

**FR-04.3** The destructor shall be `noexcept`; errors from `munmap` / `shm_unlink` shall be silently ignored (best-effort cleanup).

---

### FR-05 — Demo Application (`shmring_demo`)

**FR-05.1** The project shall include a demo binary `shmring_demo` that accepts a sub-command (e.g., `producer` / `consumer`) and a path to a `.pcap` file.

**FR-05.2** The producer process shall:
- Open the `.pcap` file via `libpcap`.
- Create the SHM segment (Owner = true).
- Write each raw packet (up to `ElementSize` bytes) into the ring buffer via `push()`.
- On EOF or SIGTERM, flush remaining packets (spin-retry is acceptable here, outside the hot path) and then exit cleanly.

**FR-05.3** The consumer process shall:
- Attach to the existing SHM segment (Owner = false).
- Drain packets via `pop()`, counting and validating them (e.g., verify packet length fields are non-zero).
- On SIGTERM or producer EOF signal, print a summary (packet count, total bytes received) and exit cleanly.

**FR-05.4** Both processes shall exit with return code `0` on normal termination and non-zero on error.

---

### FR-06 — Catch2 Test Suite

**FR-06.1** The test suite shall include single-process correctness tests covering:
- `push()` returns `true` when slots are available and `false` when the buffer is full.
- `pop()` returns `true` when data is available and `false` when the buffer is empty.
- Data integrity: bytes written by `push()` are retrieved unchanged by `pop()`.
- Boundary: exactly `Capacity` consecutive `push()` calls succeed; the `Capacity+1`-th call returns `false`.
- Boundary: exactly `Capacity` consecutive `push()`/`pop()` round-trips leave the buffer empty.

**FR-06.2** The test suite shall include at least one multi-process test (e.g., using `fork()`) that verifies end-to-end correctness across producer and consumer processes sharing the same SHM segment.

**FR-06.3** All tests shall be registered with CMake's CTest so that `ctest` discovers and runs them automatically.

---

### FR-07 — Google Benchmark Suite

**FR-07.1** The benchmark suite shall include a single-threaded (same-process) throughput benchmark measuring `push()` + `pop()` pairs per second.

**FR-07.2** The benchmark suite shall include a cross-process latency benchmark measuring the round-trip time for a single element from producer `push()` to consumer `pop()` acknowledgement.

**FR-07.3** Benchmark binaries shall be built as a separate CMake target (`shmring_bench`) and shall not be required to run during `ctest`.

---

## 5. Non-Functional Requirements

### NFR-01 — Zero Dynamic Allocation on the Hot Path

The `push()` and `pop()` implementations shall perform no heap allocation (no `new`, `delete`, `malloc`, `free`, `std::allocator`, or container operations that allocate) during their execution.

### NFR-02 — Cache-Line Alignment of Head and Tail Atomics

The head and tail atomic index members shall each be padded to occupy a separate 64-byte cache line (e.g., via `alignas(64)` and padding arrays) to eliminate false sharing between the producer and consumer.

### NFR-03 — Stable In-SHM Layout

The shared-memory region shall contain only POD or layout-compatible types. The layout shall be documented (offset and size of each field) so that it can be verified manually. No compiler-generated virtual dispatch pointers (`vptr`) or `std::` internals shall appear in the mapped region.

### NFR-04 — C++20 Standard

All source files shall compile cleanly under `-std=c++20` with warnings enabled (`-Wall -Wextra -Wpedantic`). No deprecated or platform-undefined language features shall be used.

### NFR-05 — Build Reproducibility

A clean `cmake --build` from an empty build directory shall succeed without requiring any manual steps beyond installing the listed dependencies. All third-party dependencies (Catch2, Google Benchmark) shall be resolved via Conan 2 package manager; libpcap is resolved as a documented system-package prerequisite.

---

## 6. Hard Constraints

| ID | Constraint |
|----|-----------|
| **HC-01** | Language standard: C++20 |
| **HC-02** | Synchronisation primitive: `std::atomic` with explicit `std::memory_order` annotations; no `std::mutex` or `std::lock_guard` on the `push()`/`pop()` hot path |
| **HC-03** | IPC backing: POSIX `shm_open` + `mmap`; Linux only |
| **HC-04** | Unit test framework: Catch2 (v3 preferred) |
| **HC-05** | Benchmark framework: Google Benchmark |
| **HC-06** | Build system: CMake ≥ 3.20 + Conan 2 package manager |
| **HC-07** | The ring buffer memory-ordering strategy must be derived from first principles against the C++ memory model and the rationale documented inline |

---

## 7. Assumptions

1. The target OS is Linux with a kernel that supports POSIX shared memory (`/dev/shm` or equivalent).
2. The CPU architecture is x86-64 (TSO model) during initial development; arm64 portability is not required for v1.0 but the acquire/release annotations must be correct on weakly-ordered architectures by design.
3. The system has `libpcap` (and its development headers) installed for the demo target.
4. At most one producer process and one consumer process access a given SHM segment concurrently (SPSC invariant is upheld by the caller).
5. `ElementSize` and `Capacity` are chosen such that the total SHM region fits within available `/dev/shm` space; no advisory check is required.
6. The `.pcap` file used in the demo fits in local storage and contains valid pcap-format frames.

---

## 8. Acceptance Criteria

| Criterion | Pass Condition |
|-----------|---------------|
| AC-01 | `cmake --build` completes without errors on a clean Ubuntu 22.04+ environment with the required dependencies installed |
| AC-02 | `ctest --output-on-failure` reports 0 failures |
| AC-03 | `push()` on a full buffer returns `false` without blocking (verified by test) |
| AC-04 | `pop()` on an empty buffer returns `false` without blocking (verified by test) |
| AC-05 | Multi-process Catch2 test passes: all `Capacity` elements pushed by the producer are received intact by the consumer |
| AC-06 | Google Benchmark suite runs to completion and reports non-zero throughput and latency numbers |
| AC-07 | `shmring_demo producer` + `shmring_demo consumer` process pair runs end-to-end on a sample `.pcap` file; consumer reports a packet count equal to the number of packets in the file |
| AC-08 | `valgrind --tool=helgrind` (or equivalent) reports no data-race errors on the single-process test binary |
| AC-09 | No `std::mutex` or blocking synchronisation call appears in the `push()` / `pop()` implementation (verified by code review) |

---

## 9. Open Questions

| # | Question | Owner | Priority |
|---|----------|-------|---------|
| OQ-01 | Should `ElementSize` be fixed at compile time only, or should a runtime-configurable variant be considered for v1.x? | Project owner | Low |
| OQ-02 | Is a spin-wait (`while (!push(...))`) acceptable in the demo producer on a full buffer, or must the demo apply backpressure (sleep/yield)? | Project owner | Medium |
| OQ-03 | Should the Catch2 multi-process test use `fork()` directly, or a helper script that launches two separate executables? | Project owner | Medium |
| OQ-04 | Is `libpcap` a hard dependency for the demo, or is a synthetic packet generator acceptable as a fallback when `libpcap` is unavailable? | Project owner | Low |
| OQ-05 | What is the target `Capacity` and `ElementSize` for the benchmark baseline (e.g., 4096 slots × 2048 bytes)? | Project owner | Medium |

---

*Requirements are ready for the System Architect agent to design against.*
