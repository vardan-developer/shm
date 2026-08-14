#include "common.hpp"
#include "test_common.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

using namespace Code::Buffer;
using namespace Code::Buffer::Tests;

inline constexpr char shm_name[] = "/shm-test";
constexpr BufferParam params = {shm_name};

using SHMSingleBufNoOverwriteTest = BufTest<1024 /*Number of entries*/, 256 /*size of each entry*/,
                                            false /*overwrite mode*/, params /*buffer params*/>;
using SHMMultiBufNoOverwriteTest =
    BufTestSHM<1024 /*Number of entries*/, 256 /*size of each entry*/, false /*overwrite mode*/,
               params /*buffer params*/>;

TEST_F(SHMSingleBufNoOverwriteTest, PushOnEmptyQueueSucceeds) {
    EXPECT_EQ(buf->push(Bytes<54>{23}), PUSH_STATUS::SUCCESS);
}

TEST_F(SHMSingleBufNoOverwriteTest, PopOnEmptyQueueFails) {
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY);
}

TEST_F(SHMSingleBufNoOverwriteTest, PushPopRoundTrip) {
    constexpr size_t NUM_BYTES = 129;
    ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{23}), PUSH_STATUS::SUCCESS);
    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(23)) << "data mismatch";
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMSingleBufNoOverwriteTest, UseOnlyNminus1Positions) {
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

TEST_F(SHMSingleBufNoOverwriteTest, SeqFIFOProperty) {
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

TEST_F(SHMSingleBufNoOverwriteTest, RandomFIFOProperty) {
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

TEST_F(SHMSingleBufNoOverwriteTest, PushPopVariableDataTypes) {
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

TEST_F(SHMMultiBufNoOverwriteTest, PushPopRoundTrip) {
    constexpr size_t NUM_BYTES = 129;
    ASSERT_EQ(buf1->push(Bytes<NUM_BYTES>{23}), PUSH_STATUS::SUCCESS);
    auto buf_data = buf2->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(23)) << "data mismatch";
    EXPECT_EQ(buf1->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMMultiBufNoOverwriteTest, UseOnlyNminus1Positions) {
    constexpr size_t num_elements = buf_size - 1;
    for (size_t i = 0; i < num_elements; i++) {
        ASSERT_EQ(get_random_buf()->push(Bytes<52>{uint8_t(i)}), PUSH_STATUS::SUCCESS)
            << "push #" << i;
    }
    ASSERT_EQ(get_random_buf()->push(Bytes<52>{2}), PUSH_STATUS::FULL);

    POP_STATUS pop_rc;
    size_t popped = 0;
    while ((pop_rc = get_random_buf()->pop().rc) != POP_STATUS::EMPTY) {
        if (pop_rc == POP_STATUS::SUCCESS)
            popped++;
        ASSERT_LE(popped, num_elements) << "popped more than pushed";
    }
    ASSERT_EQ(popped, num_elements) << "elements lost";
}

TEST_F(SHMMultiBufNoOverwriteTest, SeqFIFOProperty) {
    constexpr size_t NUM_BYTES = 51;
    constexpr size_t num_elements = buf_size - 1;

    std::vector<uint8_t> starts(num_elements);
    for (size_t i = 0; i < num_elements; i++) {
        starts[i] = get_random();
        ASSERT_EQ(get_random_buf()->push(Bytes<NUM_BYTES>{starts[i]}), PUSH_STATUS::SUCCESS)
            << "push #" << i;
    }
    ASSERT_EQ(get_random_buf()->push(Bytes<NUM_BYTES>{}), PUSH_STATUS::FULL);

    for (size_t i = 0; i < num_elements; i++) {
        auto buf_data = get_random_buf()->pop();
        ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS) << "pop #" << i;
        ASSERT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(starts[i])) << "data mismatch at #" << i;
    }
    ASSERT_EQ(get_random_buf()->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMMultiBufNoOverwriteTest, RandomFIFOProperty) {
    constexpr size_t NUM_BYTES = 45;
    constexpr size_t capacity = buf_size - 1;
    constexpr size_t num_operations = 100'000'000;

    size_t size = 0;
    std::queue<Bytes<NUM_BYTES>> q;
    for (size_t i = 0; i < num_operations; i++) {
        if (get_random(0, 1) == 1 && size < capacity) {
            size++;
            q.push(Bytes<NUM_BYTES>{uint8_t(i)});
            ASSERT_EQ(get_random_buf()->push(Bytes<NUM_BYTES>{uint8_t(i)}), PUSH_STATUS::SUCCESS)
                << "push #" << i;
        } else if (size > 0) {
            size--;
            auto q_data = q.front();
            q.pop();
            auto b_data = get_random_buf()->pop();
            ASSERT_EQ(b_data.rc, POP_STATUS::SUCCESS) << "pop #" << i;
            ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, q_data) << "data mismatch at #" << i;
        }
    }
    while (size--) {
        auto q_data = q.front();
        q.pop();
        auto b_data = get_random_buf()->pop();
        ASSERT_EQ(b_data.rc, POP_STATUS::SUCCESS) << "pop #" << size;
        ASSERT_EQ(Bytes<NUM_BYTES>{b_data.data}, q_data) << "data mismatch at #" << size;
    }
    ASSERT_EQ(get_random_buf()->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMMultiBufNoOverwriteTest, PushPopVariableDataTypes) {
    ASSERT_EQ(get_random_buf()->push(Bytes<59>{5}), PUSH_STATUS::SUCCESS);
    ASSERT_EQ(get_random_buf()->push(Bytes<240>{6}), PUSH_STATUS::SUCCESS);
    ASSERT_EQ(get_random_buf()->push(Bytes<22>{34}), PUSH_STATUS::SUCCESS);

    auto buf_data = get_random_buf()->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<59>{buf_data.data}.check(5)) << "data mismatch";

    buf_data = get_random_buf()->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<240>{buf_data.data}.check(6)) << "data mismatch";

    buf_data = get_random_buf()->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<22>{buf_data.data}.check(34)) << "data mismatch";
}

TEST_F(SHMMultiBufNoOverwriteTest, TwoThreadSeqStress) {
    size_t num_operations = 10'00'000;
    uint8_t max_cont_spins = 10;
    uint64_t last_seq = UINT64_MAX;
    auto spin_sleep = std::chrono::milliseconds(10);
    std::thread producer{[&] {
        uint8_t spins{0};
        while (num_operations > 0) {
            ASSERT_LE(spins, max_cont_spins) << "Producer Stalled";
            auto rc = buf1->push(uint64_t{num_operations});
            if (rc == PUSH_STATUS::SUCCESS) {
                num_operations--;
                spins = 0;
            } else {
                spins++;
                std::this_thread::sleep_for(spin_sleep);
            }
        }
    }};

    auto consumer = [&] {
        uint8_t spins{0};
        while (last_seq > 1u) {
            ASSERT_LE(spins, max_cont_spins) << "Consumer Stalled";
            auto buf_data = buf2->pop();
            ASSERT_NE(buf_data.rc, POP_STATUS::OVERWRITTEN)
                << "POP_STATUS::OVERWRITTEN in non overwrite mode";
            if (buf_data.rc == POP_STATUS::SUCCESS) {
                if (last_seq != UINT64_MAX) {
                    ASSERT_EQ(last_seq - 1u, *reinterpret_cast<uint64_t*>(buf_data.data));
                }
                last_seq = *reinterpret_cast<uint64_t*>(buf_data.data);
                spins = 0;
            } else {
                spins++;
                std::this_thread::sleep_for(spin_sleep);
            }
        }
        ASSERT_EQ(last_seq, 1u);
    };

    consumer();
    producer.join();
}
