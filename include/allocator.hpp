#pragma once

#include "common.hpp"
#include "flock.hpp"
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace Code::Buffer {

#ifdef BUFFER_TESTING_HOOKS
// test-only scheduling hooks: a test installs a callback and can pause a
// thread at a named point to force a specific interleaving. compiled out
// entirely unless BUFFER_TESTING_HOOKS is defined by the including TU.
struct TestHooks {
        static inline void (*on_point)(const char*) = nullptr;
        static void point(const char* name) {
            if (on_point)
                on_point(name);
        }
};
#define BUF_TEST_POINT(name) ::Code::Buffer::TestHooks::point(name)
#else
#define BUF_TEST_POINT(name) ((void)0)
#endif

// this class is the owner of the data it allocates,
// so any data allocated must not be freed by the user
template<size_t MaxObjSize, size_t NumSlots, bool Overwrite> class BufferAllocator {

    private:
        BufferParam params;
        std::string shm_name; // owns the name; params.shm_name points into it.
                              // stable because the class is neither copyable nor movable
        bool _shm_open;
        FLock l_shm_sh, l_shm_ex;
        uint8_t* buf;

    public:
        BufferAllocator(const BufferParam& _params = BufferParam())
            : params(_params)
            , _shm_open(false)
            , l_shm_sh(nullptr)
            , l_shm_ex(nullptr)
            , buf(nullptr) {

            if (params.shm_name != nullptr) {
                // take ownership of the name of shm
                shm_name = params.shm_name;
                params.shm_name = shm_name.c_str();
            }

            alloc_buffer();
            if (buf == nullptr) {
                buf = nullptr;
                free_buffer();
                return;
            }
        }

        BufferAllocator(BufferAllocator&& o) = delete;
        BufferAllocator& operator=(BufferAllocator&& o) = delete;
        BufferAllocator(const BufferAllocator&) = delete;
        BufferAllocator& operator=(const BufferAllocator&) = delete;

        ~BufferAllocator() { free_buffer(); }

        uint8_t* get_buf() const { return buf; }

    private:
        void sleep(std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); }

        bool setup_flock() {
            // FLock opens the file in its constructor and keeps only the fd,
            // so passing temporaries' c_str() is safe
            std::string base = "/tmp" + shm_name;
            l_shm_sh = FLock((base + ".shm.sh").c_str());
            l_shm_ex = FLock((base + ".shm.ex").c_str());
            return l_shm_sh.flock_ready() && l_shm_ex.flock_ready();
        }

        bool lock(FLock& _lock, FLOCK_TYPE _lock_type,
                  std::chrono::microseconds timeout = std::chrono::microseconds{10}) {
            if (!_lock.flock_ready())
                return false;
            return _lock.flock(_lock_type, timeout) == 0;
        }

        uint8_t* link_shm(bool creator = false) {
            size_t buffer_size = calc_buf_size();
            int shm_oflag = params.shm_oflag;
            if (creator) {
                shm_oflag |= (O_CREAT | O_EXCL);
            } else {
                shm_oflag &= ~(O_CREAT | O_EXCL);
            }
            int fd = shm_open(params.shm_name, shm_oflag, params.shm_perm);
            if (fd == -1) {
                return nullptr;
            }
            _shm_open = true;

            if (creator && ftruncate(fd, buffer_size) != 0) {
                close(fd);
                return nullptr;
            }

            // both roles hold SH from here on: the creator downgrades its probe EX
            // (contention-free, l_shm_ex is held), the attacher acquires fresh. SH
            // comes before mmap so the mapping is the LAST acquisition -- no failure
            // path can ever leak a live mapping.
            if (!lock(l_shm_sh, FLOCK_TYPE::SHARED_NB, POLL_SLEEP_TIME)) {
                close(fd);
                return nullptr;
            }

            uint8_t* _buf = static_cast<uint8_t*>(
                mmap(nullptr, buffer_size, params.mmap_prot, params.mmap_flags, fd, 0));
            close(fd);
            if (static_cast<void*>(_buf) == MAP_FAILED) {
                return nullptr;
            }
            return _buf;
        }

        void format_buf(uint8_t* buf) {
            if (buf == nullptr) {
                return;
            }
            Slot num_elm_slot(NumSlots);
            Slot magic_num(MAGIC_NUMBER);
            std::memset(buf, 0, calc_buf_size());
            memcpy(buf, static_cast<void*>(&magic_num), sizeof(Slot));
            memcpy(buf + sizeof(Slot), static_cast<void*>(&num_elm_slot), sizeof(Slot));
        }

        bool reset_shm(uint8_t*& _buf) {
            _buf = nullptr;
            if (!lock(l_shm_ex, FLOCK_TYPE::EXCLUSIVE_NB, MAX_POLL_TIMES * POLL_SLEEP_TIME)) {
                return false;
            }
            BUF_TEST_POINT("reset.serialized");
            bool creator = lock(l_shm_sh, FLOCK_TYPE::EXCLUSIVE_NB, std::chrono::microseconds{0});
            if (creator) {
                BUF_TEST_POINT("reset.will_reset");
                shm_unlink(params.shm_name);
                _buf = link_shm(true);
                if (_buf != nullptr) {
                    format_buf(_buf);
                    BUF_TEST_POINT("reset.formatted");
                }
            } else {
                _buf = link_shm();
            }

            if (_buf == nullptr) {
                // failed: drop whatever l_shm_sh state we still hold (the probe EX,
                // or the SH a partially-failed link acquired) so we leave no footprint
                (void)lock(l_shm_sh, FLOCK_TYPE::UNLOCK);
            }
            (void)lock(l_shm_ex, FLOCK_TYPE::UNLOCK);
            return _buf != nullptr;
        }

        inline size_t calc_buf_size() {
            size_t buffer_size = 4 * sizeof(Slot);
            buffer_size += NumSlots * MaxObjSize;
            if constexpr (Overwrite) { // we only need the version slots in overwrite mode
                buffer_size += sizeof(Slot) * NumSlots;
            }
            return buffer_size;
        }

        uint8_t* alloc_heap_buffer() {
            uint8_t* buf = static_cast<uint8_t*>(
                ::operator new(calc_buf_size(), std::align_val_t(cache_line_size)));
            if (buf != nullptr) {
                Slot num_elm_slot(NumSlots);
                Slot magic_num(MAGIC_NUMBER);
                memset(buf, 0, calc_buf_size());
                memcpy(buf, static_cast<void*>(&magic_num), sizeof(Slot));
                memcpy(buf + sizeof(Slot), static_cast<void*>(&num_elm_slot), sizeof(Slot));
                format_buf(buf);
            }
            return buf;
        }

        uint8_t* alloc_shm_buffer() {
            if (params.shm_name == nullptr || params.shm_name[0] != '/' || !setup_flock()) {
                return nullptr;
            }
            uint8_t* buf = nullptr;
            // reset_shm serializes all constructors on l_shm_ex. The first arrival with no
            // live users (nobody holds SH on l_shm_sh) unlinks any stale segment and creates
            // and formats a fresh one; everyone else attaches. Both paths leave here holding
            // SH on l_shm_sh, which is the liveness refcount: teardown may unlink only after
            // winning EX on it. The EX->SH downgrade happens while l_shm_ex is still held, so
            // no second constructor can slip in during the downgrade gap and reset again.
            (void)reset_shm(buf);
            return buf;
        }

        /**
         * Buffer layout (header fields are one cache-line-sized Slot each):
         *
         *   +---------------+--------------------------+-------------------------+
         *   | field         | size                     | notes                   |
         *   +---------------+--------------------------+-------------------------+
         *   | MAGIC_NUMBER  | sizeof(Slot)             | written last by the     |
         *   |               |                          | creator: marks buffer   |
         *   |               |                          | as fully initialized    |
         *   | NUM_SLOTS     | sizeof(Slot)             | capacity, power of 2    |
         *   | HEAD          | sizeof(Slot)             | producer index          |
         *   | TAIL          | sizeof(Slot)             | consumer index          |
         *   | version array | NUM_SLOTS * sizeof(Slot) | overwrite mode only     |
         *   | payload       | NUM_SLOTS * MaxObjSize   | the actual data slots   |
         *   +---------------+--------------------------+-------------------------+
         * */
        void alloc_buffer() {
            if (params.type == BUFFER_TYPE::HEAP) {
                buf = alloc_heap_buffer();
                return;
            }
            buf = alloc_shm_buffer();
        }

        void free_heap_buffer() { ::operator delete(buf, std::align_val_t(cache_line_size)); }

        int free_shm_buffer() {
            if (!_shm_open) {
                return 0;
            }
            size_t buffer_size = calc_buf_size();
            int rc = 0, err = 0;
            if (buf != nullptr) {
                rc = munmap(static_cast<void*>(buf), buffer_size);
            }
            if (rc != 0) {
                err = errno;
            }
            // if we reach here it means flock is ready
            // we take l_shm_ex so that a race cannot happen where the attacher cannot take
            // exclusive lock on l_shm_sh so takes shared because of some process exiting at that
            // point
            if (lock(l_shm_ex, FLOCK_TYPE::EXCLUSIVE_NB, std::chrono::milliseconds{0}) &&
                lock(l_shm_sh, FLOCK_TYPE::EXCLUSIVE_NB)) {
                BUF_TEST_POINT("free.reaping");
                // sole process with shm, can safely delete
                shm_unlink(params.shm_name);
            }
            lock(l_shm_sh, FLOCK_TYPE::UNLOCK);
            lock(l_shm_ex, FLOCK_TYPE::UNLOCK);
            return err;
        }

        int free_buffer() {
            if (buf == nullptr && params.type == BUFFER_TYPE::HEAP) {
                return 0;
            }
            if (params.type == BUFFER_TYPE::HEAP) {
                free_heap_buffer();
                return 0;
            }
            return free_shm_buffer();
        }
};

} // namespace Code::Buffer
