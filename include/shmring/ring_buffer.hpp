#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "shm_region.hpp"

namespace shmring {

/// In-SHM control header — exactly one cache line (64 bytes).
///
/// All cross-process accesses to `magic` use std::atomic_ref<uint32_t>
/// (C++20) to provide acquire/release ordering without embedding std::atomic
/// in the struct (preserving ABI portability, per design decision D-02).
/// `flags` is std::atomic<uint32_t> per the project specification.
struct ShmHeader {
    static constexpr uint32_t kMagic    = 0x534D5246u;  ///< "SMRF"
    static constexpr uint32_t kVersion  = 1u;
    static constexpr uint32_t kFlagDone = 0x1u;         ///< bit 0 = PRODUCER_DONE

    uint32_t              magic;         ///< written last by owner (release); consumer acquires
    uint32_t              version;       ///< layout version, currently 1
    uint32_t              element_size;  ///< max payload bytes per slot
    uint32_t              capacity;      ///< number of slots
    std::atomic<uint32_t> flags;         ///< bit 0 = PRODUCER_DONE
    uint8_t               _pad[44];      ///< pad struct to exactly 64 bytes
};
static_assert(sizeof(ShmHeader) == 64, "ShmHeader must be exactly 64 bytes");

/// Cache-line-padded atomic index — eliminates false sharing between head and tail.
struct alignas(64) CacheLinePadded64 {
    std::atomic<uint64_t> value{0};
    uint8_t               _pad[64 - sizeof(std::atomic<uint64_t>)];
};
static_assert(sizeof(CacheLinePadded64)  == 64);
static_assert(alignof(CacheLinePadded64) == 64);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

/// Lock-free SPSC ring buffer backed by POSIX shared memory.
///
/// SHM layout (offsets from region base):
///   [  0 ..  63] ShmHeader           (64 B)
///   [ 64 .. 127] head CacheLinePadded64  (64 B) — producer writes
///   [128 .. 191] tail CacheLinePadded64  (64 B) — consumer writes
///   [192 ..    ] data array            — (4 + ElementSize) × Capacity bytes
///
/// Each slot: [ uint32_t stored_len | uint8_t payload[ElementSize] ]
///
/// Capacity must be a power of two (enforced by C++20 requires clause).
template <std::size_t ElementSize, std::size_t Capacity>
    requires (ElementSize >= sizeof(uint32_t))
          && (Capacity > 0)
          && ((Capacity & (Capacity - 1)) == 0)
class ShmRingBuffer {
public:
    static constexpr std::size_t kSlotSize = sizeof(uint32_t) + ElementSize;
    static constexpr std::size_t kMask     = Capacity - 1;

    /// Total bytes the SHM segment must occupy.
    static constexpr std::size_t required_shm_size() noexcept {
        return sizeof(ShmHeader)
             + 2 * sizeof(CacheLinePadded64)
             + kSlotSize * Capacity;
    }

    /// owner=true  : creates and initialises the SHM segment.
    /// owner=false : attaches to an existing segment; validates magic, version,
    ///               element_size and capacity against the template parameters.
    /// Throws std::system_error on POSIX failure.
    /// Throws std::runtime_error on header field mismatch (ABI guard).
    explicit ShmRingBuffer(std::string_view name, bool owner)
        : region_(name, required_shm_size(), owner) {
        auto* base = static_cast<std::byte*>(region_.data());

        if (owner) {
            // Placement-new to properly start object lifetimes in SHM.
            header_ = new (base) ShmHeader{};
            head_   = new (base + sizeof(ShmHeader)) CacheLinePadded64{};
            tail_   = new (base + sizeof(ShmHeader) + sizeof(CacheLinePadded64))
                          CacheLinePadded64{};
            data_   = base + sizeof(ShmHeader) + 2 * sizeof(CacheLinePadded64);

            header_->version      = ShmHeader::kVersion;
            header_->element_size = static_cast<uint32_t>(ElementSize);
            header_->capacity     = static_cast<uint32_t>(Capacity);
            header_->flags.store(0, std::memory_order_relaxed);
            // publish magic last with release; non-owner's acquire load on magic
            // establishes happens-before for all prior field writes
            std::atomic_ref<uint32_t>(header_->magic)
                .store(ShmHeader::kMagic, std::memory_order_release);
        } else {
            header_ = std::launder(reinterpret_cast<ShmHeader*>(base));
            head_   = std::launder(reinterpret_cast<CacheLinePadded64*>(
                          base + sizeof(ShmHeader)));
            tail_   = std::launder(reinterpret_cast<CacheLinePadded64*>(
                          base + sizeof(ShmHeader) + sizeof(CacheLinePadded64)));
            data_   = base + sizeof(ShmHeader) + 2 * sizeof(CacheLinePadded64);

            // acquire pairs with owner's release store of magic; guarantees all
            // other header fields written before that store are visible here
            if (std::atomic_ref<uint32_t>(header_->magic)
                    .load(std::memory_order_acquire) != ShmHeader::kMagic)
                throw std::runtime_error("ShmRingBuffer: magic mismatch");
            if (header_->version != ShmHeader::kVersion)
                throw std::runtime_error("ShmRingBuffer: version mismatch");
            if (header_->element_size != static_cast<uint32_t>(ElementSize))
                throw std::runtime_error("ShmRingBuffer: element_size mismatch");
            if (header_->capacity != static_cast<uint32_t>(Capacity))
                throw std::runtime_error("ShmRingBuffer: capacity mismatch");
        }
    }

    ~ShmRingBuffer() noexcept = default;

    ShmRingBuffer(const ShmRingBuffer&)            = delete;
    ShmRingBuffer& operator=(const ShmRingBuffer&) = delete;

    /// Copies len bytes (len <= ElementSize) into the next available slot.
    /// Returns false immediately when the buffer is full. No heap allocation.
    [[nodiscard]] bool push(const void* src, std::size_t len) noexcept {
        // relaxed: producer owns head_; self-read needs no cross-process fence
        uint64_t h = head_->value.load(std::memory_order_relaxed);
        // acquire: pairs with pop()'s tail_.store(release); ensures slot is fully
        // read by consumer before producer overwrites it
        uint64_t t = tail_->value.load(std::memory_order_acquire);

        if (h - t >= Capacity) return false;

        std::byte* slot  = data_ + (h & kMask) * kSlotSize;
        auto       len32 = static_cast<uint32_t>(len);
        std::memcpy(slot,                    &len32, sizeof(uint32_t));
        std::memcpy(slot + sizeof(uint32_t), src,    len);

        // release: publishes all slot writes; consumer's head_.load(acquire) in
        // pop() sees both the new index and the fully-written slot data
        head_->value.store(h + 1, std::memory_order_release);
        return true;
    }

    /// Copies the oldest slot into dst; writes stored length into out_len.
    /// Returns false immediately when the buffer is empty. No heap allocation.
    [[nodiscard]] bool pop(void* dst, std::size_t& out_len) noexcept {
        // relaxed: consumer owns tail_; self-read needs no cross-process fence
        uint64_t t = tail_->value.load(std::memory_order_relaxed);
        // acquire: pairs with push()'s head_.store(release); establishes
        // happens-before so producer's memcpy bytes are visible here
        uint64_t h = head_->value.load(std::memory_order_acquire);

        if (h == t) return false;

        const std::byte* slot = data_ + (t & kMask) * kSlotSize;
        uint32_t         stored_len = 0;
        std::memcpy(&stored_len,               slot,                    sizeof(uint32_t));
        std::memcpy(dst,                       slot + sizeof(uint32_t), stored_len);
        out_len = stored_len;

        // release: signals slot is free; pairs with push()'s tail_.load(acquire)
        // — without this, producer could overwrite the slot while memcpy is still
        // in-flight on weakly-ordered CPUs (AArch64, RISC-V)
        tail_->value.store(t + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] ShmHeader*       header() noexcept       { return header_; }
    [[nodiscard]] const ShmHeader* header() const noexcept { return header_; }

private:
    ShmRegion          region_;
    ShmHeader*         header_ = nullptr;  ///< SHM offset 0
    CacheLinePadded64* head_   = nullptr;  ///< SHM offset 64  (producer writes)
    CacheLinePadded64* tail_   = nullptr;  ///< SHM offset 128 (consumer writes)
    std::byte*         data_   = nullptr;  ///< SHM offset 192
};

} // namespace shmring
