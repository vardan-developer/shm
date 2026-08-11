#pragma once

#include "common.hpp"
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace Code::Buffer {

// this class is the owner of the data it allocates,
// so any data allocated must not be freed by the user
template<size_t MaxObjSize, size_t NumSlots, bool Overwrite> class BufferAllocator {

    private:
        BufferParam params;
        bool owner;
        uint8_t* buf;

    public:
        BufferAllocator(const BufferParam& _params = BufferParam())
            : params(_params)
            , owner(false)
            , buf(nullptr) {

            alloc_buffer();
            if (buf == nullptr) {
                return;
            }
            if (!setup_buffer()) {
                free_buffer();
                buf = nullptr;
            }
        }

        BufferAllocator(BufferAllocator&& o) {
            params = o.params;
            owner = o.owner;
            buf = o.buf;
            o.buf = nullptr;
            o.owner = false;
            o.params = {};
        }

        BufferAllocator& operator=(BufferAllocator&& o) {
            if (&o == this) {
                return *this;
            }
            free_buffer();
            params = o.params;
            owner = o.owner;
            buf = o.buf;
            o.buf = nullptr;
            o.owner = false;
            o.params = {};

            return *this;
        }

        BufferAllocator(const BufferAllocator&) = delete;
        BufferAllocator& operator=(const BufferAllocator&) = delete;

        ~BufferAllocator() { free_buffer(); }

        uint8_t* get_buf() const { return buf; }

    private:
        void sleep(std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); }

        inline size_t calc_buf_size() {
            size_t buffer_size = 4 * sizeof(Slot);
            buffer_size += NumSlots * MaxObjSize;
            if constexpr (Overwrite) { // we only need the version slots in overwrite mode
                buffer_size += sizeof(Slot) * NumSlots;
            }
            return buffer_size;
        }

        uint8_t* alloc_heap_buffer() {
            owner = true;
            return static_cast<uint8_t*>(
                ::operator new(calc_buf_size(), std::align_val_t(cache_line_size)));
        }

        uint8_t* alloc_shm_buffer() {

            if ((params.shm_oflag & O_EXCL) == 0) {
                return nullptr;
            }

            // open shm and select the owner
            auto fd = shm_open(params.shm_name, params.shm_oflag, params.shm_perm);
            if (fd == -1) {
                auto err = errno;
                if (err == EEXIST) {
                    auto shm_oflag = params.shm_oflag;
                    shm_oflag = shm_oflag & ~(O_CREAT | O_EXCL);
                    fd = shm_open(params.shm_name, shm_oflag, params.shm_perm);
                    if (fd == -1) {
                        return nullptr;
                    }
                    owner = false;
                } else {
                    return nullptr;
                }
            } else {
                owner = true;
            }

            size_t size = calc_buf_size();

            // if called by creator truncate the shm accordingly,
            // for others keep polling until size is as desired
            auto resize_shm = [&]() -> int {
                if (owner) {
                    auto rc = ftruncate(fd, size);
                    if (rc != 0) {
                        return errno;
                    }
                    return 0;
                }
                struct stat file_st;
                uint8_t poll_cnt = 0;
                do {
                    sleep(POLL_SLEEP_TIME);
                    fstat(fd, &file_st);
                } while (file_st.st_size < size && ++poll_cnt < MAX_POLL_TIMES);
                if (file_st.st_size < size)
                    return 1;
                return 0;
            };

            auto rc = resize_shm();
            if (rc) {
                close(fd);
                return nullptr;
            }

            // mmap the shm in memory and if not possible to do so set buffer to null
            void* buf = static_cast<uint8_t*>(
                mmap(nullptr, size, params.mmap_prot, params.mmap_flags, fd, 0));
            if (buf == MAP_FAILED) {
                buf = nullptr;
            }
            // TODO: check close(fd) return and log failure; do NOT retry on error --
            // the fd is released even when close fails (except EBADF), and the
            // mapping in buf is unaffected either way
            close(fd);
            return static_cast<uint8_t*>(buf);
        }

        void alloc_buffer() {
            if (params.type == BUFFER_TYPE::HEAP) {
                buf = alloc_heap_buffer();
                return;
            }
            buf = alloc_shm_buffer();
        }

        void free_heap_buffer() { ::operator delete(buf, std::align_val_t(cache_line_size)); }

        int free_shm_buffer() {

            size_t buffer_size = calc_buf_size();
            int rc = munmap(static_cast<void*>(buf), buffer_size);
            if (rc != 0) {
                perror("munmap failed");
                return errno;
            }

            return 0;
        }

        int free_buffer() {
            if (buf == nullptr) {
                return 0;
            }
            if (params.type == BUFFER_TYPE::HEAP) {
                free_heap_buffer();
                return 0;
            }
            return free_shm_buffer();
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
        bool setup_buffer() {
            Slot num_elm_slot(NumSlots);
            atomic_uint magic_num(*(reinterpret_cast<uint64_t*>(buf)));
            if (owner) {
                // only ever the creator will setup the buffer no one else will overwrite it
                // when opening the buffer
                memset(buf, 0, calc_buf_size());
                memcpy(buf + sizeof(Slot), static_cast<void*>(&num_elm_slot), sizeof(Slot));
                magic_num.store(MAGIC_NUMBER, release);
            } else {
                uint8_t count = 0;
                while (magic_num.load(acquire) != MAGIC_NUMBER && count++ < MAX_POLL_TIMES) {
                    sleep(POLL_SLEEP_TIME);
                }
                if (magic_num.load(acquire) != MAGIC_NUMBER) {
                    return false;
                }
            }
            return true;
        }
};

} // namespace Code::Buffer
