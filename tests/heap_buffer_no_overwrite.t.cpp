#include "test_common.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace Code::Buffer;
using namespace Code::Buffer::Tests;

using HeapBufNoOverwriteSingleTest = HeapBufTest<1024, 256, false>;

TEST_F(HeapBufNoOverwriteSingleTest, PushOnEmptyQueueSucceeds) {
    EXPECT_EQ(buf->push(Bytes<54>{23}), PUSH_STATUS::SUCCESS);
}

TEST_F(HeapBufNoOverwriteSingleTest, PopOnEmptyQueueFails) {
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY);
}

TEST_F(HeapBufNoOverwriteSingleTest, PushPopRoundTrip) {
    constexpr size_t NUM_BYTES = 129;
    ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{23}), PUSH_STATUS::SUCCESS);
    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(23)) << "data mismatch";
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(HeapBufNoOverwriteSingleTest, UseOnlyNminus1Positions) {
    constexpr size_t num_elements = buf_size - 1;
    for (size_t i = 0; i < num_elements; i++) {
        ASSERT_EQ(buf->push(Bytes<52>{uint8_t(i)}), PUSH_STATUS::SUCCESS) << "push #" << i;
    }
    ASSERT_EQ(buf->push(Bytes<52>{2}), PUSH_STATUS::FULL);

    POP_STATUS pop_rc;
    size_t popped = 0;
    while ((pop_rc = buf->pop().rc) != POP_STATUS::EMPTY) {
        if (pop_rc == POP_STATUS::SUCCESS)
            popped++;
        ASSERT_LE(popped, num_elements) << "popped more than pushed";
    }
    ASSERT_EQ(popped, num_elements) << "elements lost";
}

TEST_F(HeapBufNoOverwriteSingleTest, SeqFIFOProperty) {
    constexpr size_t NUM_BYTES = 51;
    constexpr size_t num_elements = buf_size - 1;

    std::vector<uint8_t> starts(num_elements);
    for (size_t i = 0; i < num_elements; i++) {
        starts[i] = get_random();
        ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{starts[i]}), PUSH_STATUS::SUCCESS) << "push #" << i;
    }
    ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{}), PUSH_STATUS::FULL);

    for (size_t i = 0; i < num_elements; i++) {
        auto buf_data = buf->pop();
        ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS) << "pop #" << i;
        ASSERT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(starts[i])) << "data mismatch at #" << i;
    }
    ASSERT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(HeapBufNoOverwriteSingleTest, RandomFIFOProperty) {
    constexpr size_t NUM_BYTES = 45;
    constexpr size_t capacity = buf_size - 1;
    constexpr size_t num_operations = 100'000'000;

    size_t size = 0;
    std::queue<Bytes<NUM_BYTES>> q;
    for (size_t i = 0; i < num_operations; i++) {
        if (get_random(0, 1) == 1 && size < capacity) {
            size++;
            q.push(Bytes<NUM_BYTES>{uint8_t(i)});
            ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{uint8_t(i)}), PUSH_STATUS::SUCCESS)
                << "push #" << i;
        } else if (size > 0) {
            size--;
            auto q_data = q.front();
            q.pop();
            auto b_data = buf->pop();
            ASSERT_EQ(b_data.rc, POP_STATUS::SUCCESS) << "pop #" << i;
            ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, q_data) << "data mismatch at #" << i;
        }
    }
    while (size--) {
        auto q_data = q.front();
        q.pop();
        auto b_data = buf->pop();
        ASSERT_EQ(b_data.rc, POP_STATUS::SUCCESS) << "pop #" << size;
        ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, q_data) << "data mismatch at #" << size;
    }
    ASSERT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(HeapBufNoOverwriteSingleTest, PushPopVariableDataTypes) {
    ASSERT_EQ(buf->push(Bytes<59>{5}), PUSH_STATUS::SUCCESS);
    ASSERT_EQ(buf->push(Bytes<240>{6}), PUSH_STATUS::SUCCESS);
    ASSERT_EQ(buf->push(Bytes<22>{34}), PUSH_STATUS::SUCCESS);

    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<59>{buf_data.data}.check(5)) << "data mismatch";

    buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<240>{buf_data.data}.check(6)) << "data mismatch";

    buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<22>{buf_data.data}.check(34)) << "data mismatch";
}

TEST_F(HeapBufNoOverwriteSingleTest, TwoThreadSeqStress) {
    constexpr uint64_t N = 10'000'000;
    // every BIG_EVERY-th message is a full Bytes<200>, exercising the whole
    // slot memcpy instead of just the first word
    constexpr uint64_t BIG_EVERY = 100'000;
    constexpr uint64_t EMPTY_LIMIT = 2'000'000'000;

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        for (uint64_t seq = 0; seq < N && !stop.load(std::memory_order_relaxed);) {
            auto rc = (seq % BIG_EVERY == 0) ? buf->push(Bytes<200>{uint8_t(seq)}) : buf->push(seq);
            if (rc == PUSH_STATUS::SUCCESS)
                ++seq;
        }
    });

    // failures are recorded and asserted after join: a fatal assert here would
    // destruct a joinable thread and std::terminate
    uint64_t expected = 0;
    uint64_t consecutive_empty = 0;
    std::string err;
    while (expected < N) {
        auto d = buf->pop();
        if (d.rc == POP_STATUS::EMPTY) {
            if (++consecutive_empty > EMPTY_LIMIT) {
                err = "consumer starved";
                break;
            }
            continue;
        }
        consecutive_empty = 0;
        if (d.rc != POP_STATUS::SUCCESS) {
            err = "unexpected pop status";
            break;
        }
        if (expected % BIG_EVERY == 0) {
            if (d.data[0] != 200 || !Bytes<200>(d.data).check(uint8_t(expected))) {
                err = "data mismatch";
                break;
            }
        } else {
            uint64_t v;
            memcpy(&v, d.data, sizeof(v));
            if (v != expected) {
                err = "data mismatch";
                break;
            }
        }
        ++expected;
    }

    stop.store(true, std::memory_order_relaxed);
    producer.join();

    ASSERT_TRUE(err.empty()) << err << " at seq " << expected;
    EXPECT_EQ(expected, N);
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}
