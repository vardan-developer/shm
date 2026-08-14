#include "test_common.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Code::Buffer;
using namespace Code::Buffer::Tests;

inline constexpr char shm_name[] = "/shm-test";
constexpr BufferParam params = {shm_name};

using SHMSingleBufOverwriteTest = BufTest<1024 /*Number of entries*/, 256 /*size of each entry*/,
                                          true /*overwrite mode*/, params /*buffer params*/>;
using SHMMultiBufOverwriteTest = BufTestSHM<1024 /*Number of entries*/, 256 /*size of each entry*/,
                                            true /*overwrite mode*/, params /*buffer params*/>;

TEST_F(SHMSingleBufOverwriteTest, PushOnEmptyQueueSucceeds) {
    EXPECT_EQ(buf->push(Bytes<54>{23}), PUSH_STATUS::SUCCESS);
}

TEST_F(SHMSingleBufOverwriteTest, PopOnEmptyQueueFails) {
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY);
}

TEST_F(SHMSingleBufOverwriteTest, PushPopRoundTrip) {
    constexpr size_t NUM_BYTES = 129;
    ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{23}), PUSH_STATUS::SUCCESS);
    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(23)) << "data mismatch";
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMSingleBufOverwriteTest, PushAlwaysSucceeds) {
    size_t num_elements = buf_size * get_random() + get_random();
    for (size_t i = 0; i < num_elements; i++) {
        ASSERT_EQ(buf->push(Bytes<52>{uint8_t(i)}), PUSH_STATUS::SUCCESS) << "push #" << i;
    }
}

TEST_F(SHMSingleBufOverwriteTest, PopSucceedsAfterOverwriteStatusIfNoWrite) {
    size_t num_elements = buf_size * get_random() + get_random();
    for (size_t i = 0; i < num_elements; i++) {
        ASSERT_EQ(buf->push(Bytes<52>{uint8_t(i)}), PUSH_STATUS::SUCCESS) << "push #" << i;
    }
    ASSERT_EQ(buf->pop().rc, POP_STATUS::OVERWRITTEN);
    ASSERT_EQ(buf->pop().rc, POP_STATUS::SUCCESS) << "num_elements " << num_elements;
}

TEST_F(SHMSingleBufOverwriteTest, UseOnlyNPositions) {
    size_t num_elements = buf_size * get_random() + get_random();
    for (size_t i = 0; i < num_elements; i++) {
        ASSERT_EQ(buf->push(Bytes<52>{uint8_t(i)}), PUSH_STATUS::SUCCESS) << "push #" << i;
    }
    POP_STATUS pop_rc;
    size_t popped = 0;
    while ((pop_rc = buf->pop().rc) != POP_STATUS::EMPTY) {
        if (pop_rc == POP_STATUS::SUCCESS)
            popped++;
        ASSERT_LE(popped, buf_size) << "popped more than pushed";
    }
    ASSERT_EQ(popped, buf_size) << "elements lost";
}

TEST_F(SHMSingleBufOverwriteTest, SeqFIFOProperty) {
    constexpr size_t NUM_BYTES = 51;
    constexpr size_t num_elements = buf_size - 1;

    std::vector<uint8_t> starts(num_elements);
    for (size_t i = 0; i < num_elements; i++) {
        starts[i] = get_random();
        ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{starts[i]}), PUSH_STATUS::SUCCESS) << "push #" << i;
    }

    for (size_t i = 0; i < num_elements; i++) {
        auto buf_data = buf->pop();
        ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS) << "pop #" << i;
        ASSERT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(starts[i])) << "data mismatch at #" << i;
    }
    ASSERT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMSingleBufOverwriteTest, RandomFIFOProperty) {
    constexpr size_t NUM_BYTES = 45;
    constexpr size_t capacity = buf_size;
    constexpr size_t num_operations = 100'000'000;

    std::array<Bytes<NUM_BYTES>, capacity> arr;
    uint64_t a_head = 0, a_tail = 0, a_size = 0;
    for (size_t i = 0; i < num_operations; i++) {
        if (get_random(0, 1) == 1) {
            if (a_size == capacity)
                a_tail++; // overwrite
            else
                a_size++;
            arr[a_head % capacity] = Bytes<NUM_BYTES>{uint8_t(i)};
            a_head++;
            ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{uint8_t(i)}), PUSH_STATUS::SUCCESS)
                << "push #" << i;
        } else {
            auto a_data = arr[a_tail % capacity];
            auto b_data = buf->pop();
            if (b_data.rc != POP_STATUS::SUCCESS)
                continue;
            a_tail++;
            a_size--;
            ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, a_data) << "data mismatch at #" << i;
        }
    }
    while (true) {
        auto a_data = arr[a_tail % capacity];
        auto b_data = buf->pop();
        if (b_data.rc == POP_STATUS::EMPTY)
            break;
        if (b_data.rc == POP_STATUS::OVERWRITTEN)
            continue;
        a_tail++;
        a_size--;
        ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, a_data) << "data mismatch at #" << a_tail;
    }
    ASSERT_EQ(a_size, 0u) << "elements lost";
    ASSERT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMMultiBufOverwriteTest, PushOnEmptyQueueSucceeds) {
    EXPECT_EQ(get_random_buf()->push(Bytes<54>{23}), PUSH_STATUS::SUCCESS);
}

TEST_F(SHMMultiBufOverwriteTest, PopOnEmptyQueueFails) {
    EXPECT_EQ(get_random_buf()->pop().rc, POP_STATUS::EMPTY);
}

TEST_F(SHMMultiBufOverwriteTest, PushPopRoundTrip) {
    constexpr size_t NUM_BYTES = 129;
    ASSERT_EQ(get_random_buf()->push(Bytes<NUM_BYTES>{23}), PUSH_STATUS::SUCCESS);
    auto buf_data = get_random_buf()->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(23)) << "data mismatch";
    EXPECT_EQ(get_random_buf()->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMMultiBufOverwriteTest, PushAlwaysSucceeds) {
    size_t num_elements = buf_size * get_random() + get_random();
    for (size_t i = 0; i < num_elements; i++) {
        ASSERT_EQ(get_random_buf()->push(Bytes<52>{uint8_t(i)}), PUSH_STATUS::SUCCESS)
            << "push #" << i;
    }
}

TEST_F(SHMMultiBufOverwriteTest, PopSucceedsAfterOverwriteStatusIfNoWrite) {
    size_t num_elements = buf_size * get_random() + get_random();
    for (size_t i = 0; i < num_elements; i++) {
        ASSERT_EQ(get_random_buf()->push(Bytes<52>{uint8_t(i)}), PUSH_STATUS::SUCCESS)
            << "push #" << i;
    }
    ASSERT_EQ(get_random_buf()->pop().rc, POP_STATUS::OVERWRITTEN);
    ASSERT_EQ(get_random_buf()->pop().rc, POP_STATUS::SUCCESS) << "num_elements " << num_elements;
}

TEST_F(SHMMultiBufOverwriteTest, UseOnlyNPositions) {
    size_t num_elements = buf_size * get_random() + get_random();
    for (size_t i = 0; i < num_elements; i++) {
        ASSERT_EQ(get_random_buf()->push(Bytes<52>{uint8_t(i)}), PUSH_STATUS::SUCCESS)
            << "push #" << i;
    }
    POP_STATUS pop_rc;
    size_t popped = 0;
    while ((pop_rc = get_random_buf()->pop().rc) != POP_STATUS::EMPTY) {
        if (pop_rc == POP_STATUS::SUCCESS)
            popped++;
        ASSERT_LE(popped, buf_size) << "popped more than pushed";
    }
    ASSERT_EQ(popped, buf_size) << "elements lost";
}

TEST_F(SHMMultiBufOverwriteTest, SeqFIFOProperty) {
    constexpr size_t NUM_BYTES = 51;
    constexpr size_t num_elements = buf_size - 1;

    std::vector<uint8_t> starts(num_elements);
    for (size_t i = 0; i < num_elements; i++) {
        starts[i] = get_random();
        ASSERT_EQ(get_random_buf()->push(Bytes<NUM_BYTES>{starts[i]}), PUSH_STATUS::SUCCESS)
            << "push #" << i;
    }

    for (size_t i = 0; i < num_elements; i++) {
        auto buf_data = get_random_buf()->pop();
        ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS) << "pop #" << i;
        ASSERT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(starts[i])) << "data mismatch at #" << i;
    }
    ASSERT_EQ(get_random_buf()->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMMultiBufOverwriteTest, RandomFIFOProperty) {
    constexpr size_t NUM_BYTES = 45;
    constexpr size_t capacity = buf_size;
    constexpr size_t num_operations = 100'000'000;

    std::array<Bytes<NUM_BYTES>, capacity> arr;
    uint64_t a_head = 0, a_tail = 0, a_size = 0;
    for (size_t i = 0; i < num_operations; i++) {
        if (get_random(0, 1) == 1) {
            if (a_size == capacity)
                a_tail++; // overwrite
            else
                a_size++;
            arr[a_head % capacity] = Bytes<NUM_BYTES>{uint8_t(i)};
            a_head++;
            ASSERT_EQ(get_random_buf()->push(Bytes<NUM_BYTES>{uint8_t(i)}), PUSH_STATUS::SUCCESS)
                << "push #" << i;
        } else {
            auto a_data = arr[a_tail % capacity];
            auto b_data = get_random_buf()->pop();
            if (b_data.rc != POP_STATUS::SUCCESS)
                continue;
            a_tail++;
            a_size--;
            ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, a_data) << "data mismatch at #" << i;
        }
    }
    while (true) {
        auto a_data = arr[a_tail % capacity];
        auto b_data = get_random_buf()->pop();
        if (b_data.rc == POP_STATUS::EMPTY)
            break;
        if (b_data.rc == POP_STATUS::OVERWRITTEN)
            continue;
        a_tail++;
        a_size--;
        ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, a_data) << "data mismatch at #" << a_tail;
    }
    ASSERT_EQ(a_size, 0u) << "elements lost";
    ASSERT_EQ(get_random_buf()->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

/*
 * Loss is legal in overwrite mode, so the stress test verifies what the queue
 * still guarantees: untorn payloads, strictly increasing seqs, loss always
 * announced via OVERWRITTEN, and full drain through seq N-1 once the producer
 * stops.
 */
TEST_F(SHMMultiBufOverwriteTest, TwoThreadStress) {
    constexpr uint64_t N = 10'000'000;
    constexpr uint64_t NO_PROGRESS_LIMIT = 2'000'000'000;

    // overwrite push never blocks, so the producer always finishes on its own
    std::thread producer([&] {
        for (uint64_t seq = 0; seq < N; ++seq)
            get_random_buf()->push(StressMsg::make(seq));
    });

    // failures are recorded and asserted after join: a fatal assert here would
    // destruct a joinable thread and std::terminate
    int64_t last = -1;
    uint64_t gaps = 0, overwritten = 0, no_progress = 0;
    std::string err;

    while (last < int64_t(N) - 1) {
        auto d = get_random_buf()->pop();
        if (d.rc != POP_STATUS::SUCCESS) {
            if (d.rc == POP_STATUS::OVERWRITTEN)
                ++overwritten;
            if (++no_progress > NO_PROGRESS_LIMIT) {
                err = "consumer starved";
                break;
            }
            continue;
        }
        no_progress = 0;

        StressMsg m;
        memcpy(&m, d.data, sizeof(m));
        if (!m.valid()) {
            err = "torn payload";
            break;
        }
        if (int64_t(m.seq) <= last) {
            err = "duplicate or reordered seq " + std::to_string(m.seq);
            break;
        }
        gaps += uint64_t(int64_t(m.seq) - last) - 1;
        last = int64_t(m.seq);
    }

    producer.join();

    ASSERT_TRUE(err.empty()) << err << " at seq " << last;
    EXPECT_EQ(last, int64_t(N) - 1) << "elements lost";
    if (gaps > 0)
        EXPECT_GT(overwritten, 0u) << "loss never announced";
    EXPECT_EQ(get_random_buf()->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}
