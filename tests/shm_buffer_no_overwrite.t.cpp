#include "common.hpp"
#include "test_common.hpp"
#include <gtest/gtest.h>

using namespace Code::Buffer;
using namespace Code::Buffer::Tests;

inline constexpr char shm_name[] = "/shm-test";
constexpr BufferParam params = {shm_name};

using SHMBufNoOverwriteTest = BufTest<1024 /*Number of entries*/, 256 /*size of each entry*/,
                                      true /*overwrite mode*/, params /*buffer params*/>;
TEST_F(SHMBufNoOverwriteTest, PushOnEmptyQueueSucceeds) {
    EXPECT_EQ(buf->push(Bytes<54>{23}), PUSH_STATUS::SUCCESS);
}

TEST_F(SHMBufNoOverwriteTest, PopOnEmptyQueueFails) {
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY);
}

TEST_F(SHMBufNoOverwriteTest, PushPopRoundTrip) {
    constexpr size_t NUM_BYTES = 129;
    ASSERT_EQ(buf->push(Bytes<NUM_BYTES>{23}), PUSH_STATUS::SUCCESS);
    auto buf_data = buf->pop();
    ASSERT_EQ(buf_data.rc, POP_STATUS::SUCCESS);
    EXPECT_TRUE(Bytes<NUM_BYTES>{buf_data.data}.check(23)) << "data mismatch";
    EXPECT_EQ(buf->pop().rc, POP_STATUS::EMPTY) << "phantom message";
}

TEST_F(SHMBufNoOverwriteTest, UseOnlyNminus1Positions) {
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

TEST_F(SHMBufNoOverwriteTest, SeqFIFOProperty) {
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

TEST_F(SHMBufNoOverwriteTest, RandomFIFOProperty) {
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

TEST_F(SHMBufNoOverwriteTest, PushPopVariableDataTypes) {
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
