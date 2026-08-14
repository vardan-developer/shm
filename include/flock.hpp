#pragma once

#include <cerrno>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
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

        int flock(FLOCK_TYPE lock_type) {
            // this is a blocking call if you do not want to block use the NB versions
            if (flock_ready()) {
                int rc = -1;
                do {
                    rc = ::flock(fd, static_cast<int>(lock_type));
                } while (rc == -1 && errno == EINTR);
                if (rc < 0) {
                    return errno;
                }
                _lock_type = lock_type;
            }
            return flock_ready() ? 0 : -1; // if fd is not set return -1
        }

        FLOCK_TYPE get_lock() const { return _lock_type; }

        bool flock_ready() const { return fd >= 0; }

        ~FLock() { close(fd); }
};
} // namespace Code::Buffer
