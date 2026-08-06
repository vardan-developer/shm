#include "buffer_memory.hpp"
#include "common.hpp"
#include "spsc_buffer.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <gtest/gtest.h>
#include <optional>
#include <queue>
#include <random>
#include <stdexcept>
#include <sys/types.h>
#include <thread>
#include <vector>

class HeapBufNoOverwriteSingleTest : public testing::Test {
    protected:
        static constexpr size_t buf_size = 1024;
        static constexpr size_t MaxObjSize = 256;
        Code::Buffer::BufferAllocator<MaxObjSize, buf_size, false> allocator{};
        std::optional<Code::Buffer::SPSCBuffer<MaxObjSize>> buf;

        void SetUp() override {
            ASSERT_NE(allocator.get_buf(), nullptr) << "heap allocation failed";
            buf.emplace(allocator.get_buf());
        }
};

uint64_t get_random(uint64_t low = 0, uint64_t high = UINT8_MAX) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> distrb(low, high);
    return distrb(gen);
}

template<size_t NUM_BYTES>
    requires(NUM_BYTES < (1 << 8))
struct Bytes {
        uint8_t data[NUM_BYTES];
        Bytes(uint8_t start = 0) {
            data[0] = NUM_BYTES;
            for (uint8_t i = 0; i < NUM_BYTES - 1; i++) {
                data[i+1] = start + i;
            }
        }

        Bytes(uint8_t* ptr) {
            memcpy(this, ptr, NUM_BYTES);
            if (data[0] != NUM_BYTES)
                throw std::logic_error("Pointer to corrupt data passed");
        }

        bool check(uint8_t start = 0) const {
            for (uint8_t i = 0; i < NUM_BYTES - 1; i++) {
                if (!(data[i+1] == static_cast<uint8_t>(i + start)))
                    return false;
            }
            return true;
        }

        bool operator==(const Bytes<NUM_BYTES>& o) const { return o.check(data[1]); }

        friend void PrintTo(const Bytes& b, std::ostream* os) {
            *os << static_cast<int>(b.data[1]);
        }
};

TEST_F(HeapBufNoOverwriteSingleTest, PushOnEmptyQueueSucceeds) {
    constexpr size_t NUM_BYTES = 54;
    Bytes<NUM_BYTES> data(23);
    EXPECT_EQ(buf->push(data), Code::Buffer::PUSH_STATUS::SUCCESS);
}

TEST_F(HeapBufNoOverwriteSingleTest, PopOnEmptyQueueFails) {
    EXPECT_EQ(buf->pop().rc, Code::Buffer::POP_STATUS::EMPTY);
}

TEST_F(HeapBufNoOverwriteSingleTest, PushPopRoundTrip) {
    constexpr size_t NUM_BYTES = 129;
    Bytes<NUM_BYTES> data_push(23);
    ASSERT_EQ(buf->push(data_push), Code::Buffer::PUSH_STATUS::SUCCESS)
        << "Push on empty buffer failed";
    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, Code::Buffer::POP_STATUS::SUCCESS)
        << "Pop on non empty non overwrite buffer failed";
    Bytes<NUM_BYTES> data_pop(buf_data.data);
    EXPECT_EQ(data_pop.check(23), true) << "Push and Pop values are divergent";
    EXPECT_EQ(buf->pop().rc, Code::Buffer::POP_STATUS::EMPTY)
        << "On a single push multiple elements pushed in buffer";
}

TEST_F(HeapBufNoOverwriteSingleTest, UseOnlyNminus1Positions) {
    auto num_elements = buf_size - 1;
    for (int i = 0; i < num_elements; i++) {
        auto rc = buf->push(Bytes<52>{uint8_t(i)});
        ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::SUCCESS)
            << "Push on a non full non overwrite buffer failed";
    }
    auto rc = buf->push(Bytes<52>{2});
    ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::FULL)
        << "Only buf_size - 1 slots should be usable but we are able to use buf_size";
    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, Code::Buffer::POP_STATUS::SUCCESS)
        << "Pop on non empty non overwrite buffer failed";
    rc = buf->push(Bytes<124>{uint8_t(9)});
    ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::SUCCESS)
        << "Push on a non full non overwrite buffer failed";
}

TEST_F(HeapBufNoOverwriteSingleTest, SeqFIFOProperty) {

    constexpr size_t NUM_BYTES = 51;
    constexpr size_t num_elements = buf_size - 1;
    std::vector<uint8_t> starts(num_elements);
    for (int i = 0; i < num_elements; i++) {
        starts[i] = get_random();
        auto rc = buf->push(Bytes<NUM_BYTES>{starts[i]});
        ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::SUCCESS)
            << "Push on a non full non overwrite buffer failed";
    }

    auto rc = buf->push(Bytes<NUM_BYTES>{});
    ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::FULL)
        << "Push of a full non overwrite buffer succeeded";

    for (int i = 0; i < num_elements; i++) {
        auto buf_data = buf->pop();
        ASSERT_EQ(buf_data.rc, Code::Buffer::POP_STATUS::SUCCESS)
            << "Pop on a non empty non overwrite buffer failed";
        Bytes<NUM_BYTES> data_pop(buf_data.data);
        ASSERT_EQ(data_pop.check(starts[i]), true)
            << "FIFO ordering is not maintained in buffer " << i;
    }

    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, Code::Buffer::POP_STATUS::EMPTY)
        << "Pop on empty queue should return POP_STATUS::EMPTY";
}

TEST_F(HeapBufNoOverwriteSingleTest, RandomFIFOProperty) {
    constexpr size_t NUM_BYTES = 45;
    constexpr size_t capacity = buf_size - 1;
    constexpr size_t num_operations = 10000000;

    size_t size = 0;
    std::queue<Bytes<NUM_BYTES>> q;
    for (int i = 0; i < num_operations; i++) {
        auto r_num = get_random(0, 1);
        if (r_num == 1 && size < capacity) {
            // push
            size++;
            q.push(Bytes<NUM_BYTES>{uint8_t(i)});
            auto rc = buf->push(Bytes<NUM_BYTES>{uint8_t(i)});
            ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::SUCCESS);
        } else if (size > 0) {
            size--;
            auto q_data = q.front();
            q.pop();
            auto b_data = buf->pop();
            ASSERT_EQ(b_data.rc, Code::Buffer::POP_STATUS::SUCCESS)
                << "Pop on a non empty non overwrite buffer failed";
            ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, q_data)
                << "FIFO ordering is not maintained in buffer";
        }
    }
    while (size--) {
        auto q_data = q.front();
        auto b_data = buf->pop();
        q.pop();
        ASSERT_EQ(b_data.rc, Code::Buffer::POP_STATUS::SUCCESS)
            << "Pop on a non empty non overwrite buffer failed";
        ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, q_data)
            << "FIFO ordering is not maintained in buffer";
    }
    auto b_data = buf->pop();
    ASSERT_EQ(b_data.rc, Code::Buffer::POP_STATUS::EMPTY)
        << "Pop on empty queue should return POP_STATUS::EMPTY";
}

TEST_F(HeapBufNoOverwriteSingleTest, PushPosVariableDataTypes) {
    auto rc = buf->push(Bytes<59>{5});
    ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::SUCCESS)
        << "Push on a non full non overwrite buffer failed";
    rc = buf->push(Bytes<240>{6});
    ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::SUCCESS)
        << "Push on a non full non overwrite buffer failed";
    rc = buf->push(Bytes<22>{34});
    ASSERT_EQ(rc, Code::Buffer::PUSH_STATUS::SUCCESS)
        << "Push on a non full non overwrite buffer failed";

    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, Code::Buffer::POP_STATUS::SUCCESS)
        << "Pop on non empty non overwrite buffer failed";
    EXPECT_NO_THROW(Bytes<59>{buf_data.data});
    EXPECT_EQ(Bytes<59>{buf_data.data}.check(5), true);

    buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, Code::Buffer::POP_STATUS::SUCCESS)
        << "Pop on non empty non overwrite buffer failed";
    EXPECT_NO_THROW(Bytes<240>{buf_data.data});
    EXPECT_EQ(Bytes<240>{buf_data.data}.check(6), true);

    buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, Code::Buffer::POP_STATUS::SUCCESS)
        << "Pop on non empty non overwrite buffer failed";
    EXPECT_NO_THROW(Bytes<22>{buf_data.data});
    EXPECT_EQ(Bytes<22>{buf_data.data}.check(34), true);
}

TEST_F(HeapBufNoOverwriteSingleTest, TwoThreadSeqStress) {
    constexpr uint64_t N = 10'000'000;
    // every BIG_EVERY-th message is a full Bytes<200> pattern instead of a bare
    // uint64_t, so the whole-slot memcpy is exercised, not just the first word
    constexpr uint64_t BIG_EVERY = 100'000;
    // ~seconds of consecutive empty polls with no producer progress => hang, bail out
    constexpr uint64_t EMPTY_LIMIT = 2'000'000'000;

    std::atomic<bool> stop{false};

    std::thread producer([&] {
        for (uint64_t seq = 0; seq < N && !stop.load(std::memory_order_relaxed);) {
            auto rc = (seq % BIG_EVERY == 0)
                          ? buf->push(Bytes<200>{uint8_t(seq)})
                          : buf->push(seq);
            if (rc == Code::Buffer::PUSH_STATUS::SUCCESS)
                ++seq;
            // FULL -> spin and retry; consumer is draining concurrently
        }
    });

    // consumer runs on the main test thread; failures are recorded and asserted
    // after the producer is joined, so the test never std::terminates on a
    // destructed joinable thread
    uint64_t expected = 0;
    uint64_t consecutive_empty = 0;
    std::string err;
    while (expected < N) {
        auto d = buf->pop();
        if (d.rc == Code::Buffer::POP_STATUS::EMPTY) {
            if (++consecutive_empty > EMPTY_LIMIT) {
                err = "consumer starved: producer made no progress";
                break;
            }
            continue;
        }
        consecutive_empty = 0;
        if (d.rc != Code::Buffer::POP_STATUS::SUCCESS) {
            err = "pop returned unexpected status";
            break;
        }
        if (expected % BIG_EVERY == 0) {
            if (d.data[0] != 200) {
                err = "big message marker corrupt (torn copy?)";
                break;
            }
            Bytes<200> b(d.data);
            if (!b.check(uint8_t(expected))) {
                err = "big message payload corrupt";
                break;
            }
        } else {
            uint64_t v;
            memcpy(&v, d.data, sizeof(v));
            if (v != expected) {
                err = "sequence mismatch: got " + std::to_string(v);
                break;
            }
        }
        ++expected;
    }

    stop.store(true, std::memory_order_relaxed);
    producer.join();

    ASSERT_TRUE(err.empty()) << err << " at seq " << expected;
    EXPECT_EQ(expected, N);

    // producer pushed exactly N and we consumed exactly N: queue must be empty
    EXPECT_EQ(buf->pop().rc, Code::Buffer::POP_STATUS::EMPTY)
        << "phantom message after all " << N << " were consumed";
}
