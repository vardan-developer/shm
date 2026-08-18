#pragma once

#include "common.hpp"
#include <cerrno>
#include <chrono>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
namespace Code::Buffer {

enum class FLOCK_TYPE : int {
    EXCLUSIVE = LOCK_EX,
    SHARED = LOCK_SH,
    UNLOCK = LOCK_UN,
    EXCLUSIVE_NB = LOCK_EX | LOCK_NB,
    SHARED_NB = LOCK_SH | LOCK_NB
};

class FLock {
    private:
        int fd;
        FLOCK_TYPE _lock_type;

        FLOCK_TYPE strip_NB(FLOCK_TYPE lock_type) {
            int stripped_lock = static_cast<int>(lock_type) & ~LOCK_NB;
            return static_cast<FLOCK_TYPE>(stripped_lock);
        }

        bool is_NB(FLOCK_TYPE lock_type) { return static_cast<int>(lock_type) & LOCK_NB; }

    public:
        FLock(const FLock& o) = delete;
        FLock& operator=(const FLock& o) = delete;

        explicit FLock(const char* file)
            : fd{-1}
            , _lock_type{FLOCK_TYPE::UNLOCK} {
            if (file != nullptr) {
                fd = open(file, O_CREAT | O_RDONLY, 0644);
            }
        }

        FLock(FLock&& o) {
            if (this == &o)
                return;
            fd = o.fd;
            _lock_type = o._lock_type;
            o.fd = -1;
            o._lock_type = FLOCK_TYPE::UNLOCK;
        }

        FLock& operator=(FLock&& o) {
            if (this == &o)
                return *this;
            close(fd);
            fd = o.fd;
            _lock_type = o._lock_type;
            o.fd = -1;
            o._lock_type = FLOCK_TYPE::UNLOCK;
            return *this;
        }

        int flock(FLOCK_TYPE lock_type,
                  std::chrono::microseconds timeout = std::chrono::microseconds{5}) {
            // this is a blocking call, if you do not want to block use the NB versions
            // with timeout
            if (flock_ready()) {

                auto deadline = std::chrono::steady_clock::now() + timeout;
                auto retry = [&] {
                    if (std::chrono::steady_clock::now() >= deadline)
                        return false;
                    bool _retry = errno == EINTR || (is_NB(lock_type) && errno == EWOULDBLOCK);
                    if (_retry)
                        std::this_thread::sleep_for(POLL_SLEEP_TIME);
                    return _retry;
                };

                int rc = -1;
                do {
                    rc = ::flock(fd, static_cast<int>(lock_type));
                } while (rc == -1 && retry());

                if (rc < 0) {
                    _lock_type = FLOCK_TYPE::UNLOCK;
                    return errno;
                }
                _lock_type = strip_NB(lock_type);
            }
            return flock_ready() ? 0 : -1; // if fd is not set return -1
        }

        FLOCK_TYPE get_lock() const { return _lock_type; }

        bool flock_ready() const { return fd >= 0; }

        ~FLock() { close(fd); }
};
} // namespace Code::Buffer
