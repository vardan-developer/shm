#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <type_traits>

namespace Code::Buffer {

template<typename T, size_t MaxObjSize>
concept PushableObject = std::is_trivially_copyable_v<T> && (sizeof(T) <= MaxObjSize);
template<size_t MaxObjSize>
concept DesiredSize =
    MaxObjSize == 256 || MaxObjSize == 512 || MaxObjSize == 1024 || MaxObjSize == 4196;

inline constexpr auto relaxed = std::memory_order_relaxed;
inline constexpr auto acquire = std::memory_order_acquire;
inline constexpr auto release = std::memory_order_release;
inline constexpr size_t atomic_alignment = std::atomic_ref<uint64_t>::required_alignment;
inline constexpr uint64_t MAGIC_NUMBER = 0x82efe05fb70ec3a7;
inline constexpr uint8_t MAX_POLL_TIMES = 50;
inline constexpr std::chrono::milliseconds POLL_SLEEP_TIME{20};

#ifndef __cpp_lib_hardware_interference_size
inline constexpr auto cache_line_size = 64;
#else
inline constexpr auto cache_line_size = std::hardware_destructive_interference_size;
#endif

enum class POP_STATUS : uint8_t { SUCCESS, EMPTY, OVERWRITTEN };
enum class PUSH_STATUS : uint8_t { SUCCESS, FULL };
enum class BUFFER_TYPE : uint8_t { SHM, HEAP };

using atomic_uint = std::atomic_ref<uint64_t>;

template<size_t MaxObjSize> struct BufData {
        uint8_t data[MaxObjSize];
        POP_STATUS rc;
};

struct alignas(cache_line_size) Slot {
        uint64_t data;
};
static_assert(alignof(Slot) % atomic_alignment == 0);
static_assert(sizeof(Slot) >= sizeof(atomic_uint));

struct BufferParam {
        BUFFER_TYPE type;

        // If BUFFER_TYPE is SHM
        const char* shm_name;
        int shm_oflag = 0;
        mode_t shm_perm = 0;

        int mmap_prot = 0;
        int mmap_flags = 0;

        constexpr BufferParam()
            : type(BUFFER_TYPE::HEAP)
            , shm_name(nullptr)
            , shm_oflag(0)
            , shm_perm(mode_t(0))
            , mmap_prot(0)
            , mmap_flags(0) {}

        constexpr BufferParam(const char* _shm_name, int _shm_oflag = O_RDWR | O_CREAT | O_EXCL,
                    mode_t _shm_perm = 0600, int _mmap_prot = PROT_READ | PROT_WRITE,
                    int _mmap_flags = MAP_SHARED)
            : type(BUFFER_TYPE::SHM)
            , shm_name(_shm_name)
            , shm_oflag(_shm_oflag)
            , shm_perm(_shm_perm)
            , mmap_prot(_mmap_prot)
            , mmap_flags(_mmap_flags) {}
};

} // namespace Code::Buffer
