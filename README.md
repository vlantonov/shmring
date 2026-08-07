# shmring — Lock-free SPSC ring buffer over POSIX shared memory (C++20)

## Features

- Single-producer / single-consumer lock-free ring buffer
- POSIX `shm_open`/`mmap` backing — IPC across separate processes
- `std::atomic` with explicit acquire/release ordering (designed from first principles against the C++ memory model)
- Header-only library (`include/shmring/`)
- Zero heap allocation on the push/pop hot path
- Catch2 unit tests (13 test cases including multi-process fork tests)
- Google Benchmark suite
- Optional pcap demo: pipes raw packets between two processes

## Requirements

| Tool | Version |
|------|---------|
| CMake | ≥ 3.20 |
| C++ standard | C++20 |
| Conan | 2 |
| Compiler | GCC / Clang |
| OS | Linux (POSIX `shm_open`) |

## Build

```bash
# 1. Install Conan 2 (skip if already installed)
pip install conan
conan profile detect

# 2. Install dependencies
conan install . --output-folder=build --build=missing --deployer=full_deploy

# 3. Configure and build
cmake -S . -B build \
      -DCMAKE_TOOLCHAIN_FILE=build/build/Release/generators/conan_toolchain.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 4. Run tests
ctest --test-dir build --output-on-failure

# 5. Run benchmarks (optional)
./build/benchmarks/shmring_bench --benchmark_format=console
```

## Demo

The demo ships two binaries that communicate through the ring buffer in separate processes. The producer reads raw packets from a `.pcap` file (or generates synthetic packets if `libpcap` is absent) and pushes them into the shared-memory ring buffer. The consumer attaches to the same segment, pops packets, and prints throughput statistics.

Run the consumer first so the SHM segment exists before the producer writes to it:

```bash
# Terminal 1 — consumer (attaches to existing segment)
./build/src/demo/shmring_demo_consumer
```

```bash
# Terminal 2 — producer (creates the segment)
./build/src/demo/shmring_demo_producer [packets.pcap]
```

## API Quick Reference

```cpp
// Class template — Capacity must be a power of two
template <std::size_t ElementSize, std::size_t Capacity>
class shmring::ShmRingBuffer;

// Construction
//   owner = true  → creates and initialises the SHM segment (shm_open O_CREAT)
//   owner = false → attaches to an existing segment; validates ABI fields
//   Throws std::system_error on POSIX failure, std::runtime_error on header mismatch.
explicit ShmRingBuffer(std::string_view name, bool owner);

// Push — copies len bytes (len ≤ ElementSize) into the next available slot.
// Returns false immediately when the buffer is full. No heap allocation.
[[nodiscard]] bool push(const void* src, std::size_t len) noexcept;

// Pop — copies the oldest slot into dst and writes stored length into out_len.
// Returns false immediately when the buffer is empty. No heap allocation.
[[nodiscard]] bool pop(void* dst, std::size_t& out_len) noexcept;
```

### SHM size formula

```
required bytes = 192 + (4 + ElementSize) × Capacity
```

(`192` = 64-byte header + two 64-byte cache-line-padded index slots)

## Project Structure

```
shmring/
├── include/shmring/        # Header-only library
│   ├── ring_buffer.hpp     # ShmRingBuffer<ElementSize, Capacity>
│   └── shm_region.hpp      # RAII shm_open / mmap wrapper
├── src/demo/               # Producer and consumer demo binaries
├── tests/                  # Catch2 correctness suite
├── benchmarks/             # Google Benchmark suite
├── cmake/                  # FindPCAP.cmake helper module
├── docs/                   # SRS and design documents
├── CMakeLists.txt
├── conanfile.py
└── VERSION
```

## License

MIT — see [LICENSE](LICENSE).
