#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shmring {

/// RAII wrapper around a POSIX shared memory segment.
///
/// Constructing with owner=true creates the segment (shm_open O_CREAT),
/// truncates it to the requested size, and maps it. The destructor calls
/// shm_unlink in addition to munmap.
///
/// Constructing with owner=false opens an existing segment and maps it.
/// The destructor only calls munmap.
///
/// Throws std::system_error (with errno) on shm_open / ftruncate / mmap failure.
class ShmRegion {
public:
    ShmRegion(std::string_view name, std::size_t size, bool owner)
        : size_(size), name_(name), owner_(owner) {
        int flags = O_RDWR;
        if (owner_) flags |= O_CREAT | O_TRUNC;

        int fd = ::shm_open(name_.c_str(), flags, 0600);
        if (fd == -1)
            throw std::system_error(errno, std::system_category(), "shm_open");

        if (owner_) {
            if (::ftruncate(fd, static_cast<off_t>(size_)) == -1) {
                int err = errno;
                ::close(fd);
                ::shm_unlink(name_.c_str());
                throw std::system_error(err, std::system_category(), "ftruncate");
            }
        }

        ptr_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        int err = errno;
        ::close(fd);  // fd may be closed after mmap; mapping persists

        if (ptr_ == MAP_FAILED) {
            if (owner_) ::shm_unlink(name_.c_str());
            throw std::system_error(err, std::system_category(), "mmap");
        }
    }

    ~ShmRegion() noexcept {
        if (ptr_ != MAP_FAILED) ::munmap(ptr_, size_);
        if (owner_ && !name_.empty()) ::shm_unlink(name_.c_str());
    }

    ShmRegion(const ShmRegion&)            = delete;
    ShmRegion& operator=(const ShmRegion&) = delete;

    ShmRegion(ShmRegion&& other) noexcept
        : ptr_(other.ptr_), size_(other.size_),
          name_(std::move(other.name_)), owner_(other.owner_) {
        other.ptr_   = MAP_FAILED;
        other.size_  = 0;
        other.owner_ = false;
    }

    ShmRegion& operator=(ShmRegion&& other) noexcept {
        if (this != &other) {
            if (ptr_ != MAP_FAILED) ::munmap(ptr_, size_);
            if (owner_ && !name_.empty()) ::shm_unlink(name_.c_str());
            ptr_   = other.ptr_;
            size_  = other.size_;
            name_  = std::move(other.name_);
            owner_ = other.owner_;
            other.ptr_   = MAP_FAILED;
            other.size_  = 0;
            other.owner_ = false;
        }
        return *this;
    }

    [[nodiscard]] void*       data()  noexcept       { return ptr_; }
    [[nodiscard]] const void* data()  const noexcept { return ptr_; }
    [[nodiscard]] std::size_t size()  const noexcept { return size_; }

private:
    void*       ptr_   = MAP_FAILED;
    std::size_t size_  = 0;
    std::string name_;
    bool        owner_ = false;
};

} // namespace shmring
