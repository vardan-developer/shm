#pragma once

#include "buffer_memory.hpp"
#include "common.hpp"
#include "spsc_buffer.hpp"
#include <alloca.h>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace Code::Buffer::Tests {

inline uint64_t get_random(uint64_t, uint64_t);

template<size_t BufSize, size_t ObjSize, bool Overwrite = false, BufferParam Params = BufferParam()>
class BufTest : public testing::Test {
    protected:
        static constexpr size_t buf_size = BufSize;
        static constexpr size_t MaxObjSize = ObjSize;
        using Queue =
            std::conditional_t<Overwrite, SPSCBufferOverwrite<ObjSize>, SPSCBuffer<ObjSize>>;
        using Alloc = BufferAllocator<ObjSize, BufSize, Overwrite>;

        BufferParam params{Params};
        std::optional<Alloc> allocator;
        std::optional<Queue> buf;

        BufTest() {
            if (params.shm_name) {
                std::string _tmp = params.shm_name + std::to_string(get_random(0, 10002100312312));
                params.shm_name = _tmp.c_str();
            }
            allocator.emplace(params);
        }

        void SetUp() override {
            ASSERT_NE(allocator->get_buf(), nullptr) << "memory allocation failed";
            buf.emplace(allocator->get_buf());
        }
};

template<size_t BufSize, size_t ObjSize, bool Overwrite = false,
         BufferParam Params = BufferParam("my-shm-perm")>
class BufTestSHM : public testing::Test {
    protected:
        static constexpr size_t buf_size = BufSize;
        static constexpr size_t MaxObjSize = ObjSize;
        using Queue =
            std::conditional_t<Overwrite, SPSCBufferOverwrite<ObjSize>, SPSCBuffer<ObjSize>>;
        using Alloc = BufferAllocator<ObjSize, BufSize, Overwrite>;

        BufferParam params{Params};
        std::optional<Alloc> allocator1, allocator2;
        std::optional<Queue> buf1, buf2;

        BufTestSHM() {
            if (params.shm_name == nullptr || params.type != BUFFER_TYPE::SHM) {
                return;
            }
            std::string _tmp = params.shm_name + std::to_string(get_random(0, 10002100312312));
            params.shm_name = _tmp.c_str();
            allocator1.emplace(params);
            allocator2.emplace(params);
        }

        void SetUp() override {
            ASSERT_TRUE(allocator1.has_value())
                << "Allocator not initialized, either type is not shm or shm_name is null";
            ASSERT_TRUE(allocator2.has_value())
                << "Allocator not initialized, either type is not shm or shm_name is null";
            ASSERT_NE(params.shm_name, nullptr) << "SHM name cannot be null";
            ASSERT_NE(allocator1->get_buf(), nullptr) << "memory allocation failed";
            ASSERT_NE(allocator2->get_buf(), nullptr) << "memory allocation failed";
            buf1.emplace(allocator1->get_buf());
            buf2.emplace(allocator2->get_buf());
        }

        std::optional<Queue>& get_random_buf() {
            if (get_random(0, 1))
                return buf1;
            return buf2;
        }
};

inline uint64_t get_random(uint64_t low = 0, uint64_t high = UINT8_MAX) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> distrb(low, high);
    return distrb(gen);
}

// self-describing payload: data[0] = size marker (validated when constructed
// from raw queue bytes), data[1..] = start-derived pattern
template<size_t NUM_BYTES>
    requires(NUM_BYTES >= 2 && NUM_BYTES < (1 << 8))
struct Bytes {
        uint8_t data[NUM_BYTES];

        Bytes(uint8_t start = 0) {
            data[0] = NUM_BYTES;
            for (uint8_t i = 0; i < NUM_BYTES - 1; i++) {
                data[i + 1] = start + i;
            }
        }

        Bytes(uint8_t* ptr) {
            memcpy(this, ptr, NUM_BYTES);
            if (data[0] != NUM_BYTES)
                throw std::logic_error("Pointer to corrupt data passed");
        }

        bool check(uint8_t start = 0) const {
            for (uint8_t i = 0; i < NUM_BYTES - 1; i++) {
                if (!(data[i + 1] == static_cast<uint8_t>(i + start)))
                    return false;
            }
            return true;
        }

        bool operator==(const Bytes<NUM_BYTES>& o) const { return o.check(data[1]); }

        friend void PrintTo(const Bytes& b, std::ostream* os) {
            *os << static_cast<int>(b.data[1]);
        }
};

// stress payload: seq at both ends, hash-derived pattern between, so a torn
// copy cannot pass valid() (start+i patterns collide for seqs 256 apart)
struct StressMsg {
        uint64_t seq;
        uint8_t pattern[40];
        uint64_t seq_back;

        static uint64_t mix(uint64_t x) { // splitmix64 finalizer
            x += 0x9E3779B97F4A7C15ULL;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            return x ^ (x >> 31);
        }

        static StressMsg make(uint64_t s) {
            StressMsg m;
            m.seq = s;
            m.seq_back = s;
            uint64_t h = mix(s);
            for (size_t i = 0; i < sizeof(m.pattern); i++)
                m.pattern[i] = uint8_t(h >> (8 * (i % 8))) ^ uint8_t(i);
            return m;
        }

        bool valid() const {
            if (seq != seq_back)
                return false;
            uint64_t h = mix(seq);
            for (size_t i = 0; i < sizeof(pattern); i++)
                if (pattern[i] != (uint8_t(h >> (8 * (i % 8))) ^ uint8_t(i)))
                    return false;
            return true;
        }
};

} // namespace Code::Buffer::Tests
