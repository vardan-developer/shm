// must be defined before any include that pulls in allocator.hpp
#define BUFFER_TESTING_HOOKS
#include "test_common.hpp"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace Code::Buffer;
using namespace Code::Buffer::Tests;

namespace {

constexpr size_t OBJ = 64;
constexpr size_t SLOTS = 16;
using Alloc = BufferAllocator<OBJ, SLOTS, false>;

// pauses allocator threads at named BUF_TEST_POINTs until the test releases them
class SyncPoints {
    public:
        void block_at(const std::string& p) {
            std::lock_guard<std::mutex> l(m);
            blocked[p] = true;
        }

        void release(const std::string& p) {
            std::lock_guard<std::mutex> l(m);
            blocked[p] = false;
            cv.notify_all();
        }

        void release_all() {
            std::lock_guard<std::mutex> l(m);
            for (auto& kv : blocked)
                kv.second = false;
            cv.notify_all();
        }

        bool wait_arrival(const std::string& p, std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> l(m);
            return cv.wait_for(l, timeout, [&] { return arrived[p] > 0; });
        }

        void reset() {
            std::lock_guard<std::mutex> l(m);
            blocked.clear();
            arrived.clear();
        }

        void on_point(const char* p) {
            std::unique_lock<std::mutex> l(m);
            arrived[p]++;
            cv.notify_all();
            cv.wait(l, [&] { return !blocked[p]; });
        }

    private:
        std::mutex m;
        std::condition_variable cv;
        std::unordered_map<std::string, bool> blocked;
        std::unordered_map<std::string, int> arrived;
};

SyncPoints g_points;
void hook_trampoline(const char* p) {
    g_points.on_point(p);
}

bool shm_name_exists(const std::string& name) {
    int fd = shm_open(name.c_str(), O_RDWR, 0600);
    if (fd == -1)
        return false;
    close(fd);
    return true;
}

uint64_t read_slot(const uint8_t* buf, size_t idx) {
    uint64_t v;
    memcpy(&v, buf + idx * sizeof(Slot), sizeof(v));
    return v;
}

class AllocatorTest : public testing::Test {
    protected:
        std::string name;

        void SetUp() override {
            name = "/alloc-test-" + std::to_string(get_random(0, 1'000'000'000));
            g_points.reset();
            TestHooks::on_point = hook_trampoline;
        }

        void TearDown() override {
            g_points.release_all(); // never leave a paused thread stuck on failure
            TestHooks::on_point = nullptr;
        }
};

TEST_F(AllocatorTest, ConcurrentConstructAllSucceed) {
    constexpr int N = 8;
    std::vector<std::unique_ptr<Alloc>> allocs(N);
    std::atomic<int> ready{0};

    std::vector<std::thread> ts;
    for (int i = 0; i < N; i++) {
        ts.emplace_back([&, i] {
            ready++;
            while (ready.load() < N) {
            } // start roughly together
            allocs[i] = std::make_unique<Alloc>(BufferParam(name.c_str()));
        });
    }
    for (auto& t : ts)
        t.join();

    for (int i = 0; i < N; i++) {
        ASSERT_NE(allocs[i]->get_buf(), nullptr) << "allocator #" << i;
        EXPECT_EQ(read_slot(allocs[i]->get_buf(), 0), MAGIC_NUMBER) << "allocator #" << i;
        EXPECT_EQ(read_slot(allocs[i]->get_buf(), 1), SLOTS) << "allocator #" << i;
    }

    // all mappings must be views of the same segment
    uint8_t* payload = allocs[0]->get_buf() + 4 * sizeof(Slot);
    *payload = 0xAB;
    for (int i = 1; i < N; i++) {
        EXPECT_EQ(*(allocs[i]->get_buf() + 4 * sizeof(Slot)), 0xAB) << "allocator #" << i;
    }

    allocs.clear();
    EXPECT_FALSE(shm_name_exists(name)) << "segment not unlinked after last destructor";
}

TEST_F(AllocatorTest, LastDestructorUnlinks) {
    auto a1 = std::make_unique<Alloc>(BufferParam(name.c_str()));
    auto a2 = std::make_unique<Alloc>(BufferParam(name.c_str()));
    ASSERT_NE(a1->get_buf(), nullptr);
    ASSERT_NE(a2->get_buf(), nullptr);

    a1.reset();
    EXPECT_TRUE(shm_name_exists(name)) << "unlinked while a user was still attached";
    a2.reset();
    EXPECT_FALSE(shm_name_exists(name)) << "segment not unlinked after last destructor";
}

TEST_F(AllocatorTest, StaleSegmentIsReset) {
    // fabricate a leftover segment from a "crashed" generation: right name and
    // size, valid magic, garbage indices, and no lock holders
    size_t size = 4 * sizeof(Slot) + SLOTS * OBJ;
    int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    ASSERT_NE(fd, -1);
    ASSERT_EQ(ftruncate(fd, size), 0);
    auto* stale =
        static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    close(fd);
    ASSERT_NE(static_cast<void*>(stale), MAP_FAILED);
    memset(stale, 0xFF, size);
    uint64_t magic = MAGIC_NUMBER;
    memcpy(stale, &magic, sizeof(magic));
    munmap(stale, size);

    Alloc a{BufferParam(name.c_str())};
    ASSERT_NE(a.get_buf(), nullptr);
    EXPECT_EQ(read_slot(a.get_buf(), 0), MAGIC_NUMBER);
    EXPECT_EQ(read_slot(a.get_buf(), 1), SLOTS) << "stale num_slots not reset";
    EXPECT_EQ(read_slot(a.get_buf(), 2), 0u) << "stale head not reset";
    EXPECT_EQ(read_slot(a.get_buf(), 3), 0u) << "stale tail not reset";
}

TEST_F(AllocatorTest, AttacherWaitsForResetToFinish) {
    g_points.block_at("reset.formatted");

    std::unique_ptr<Alloc> a, b;
    std::thread ta([&] { a = std::make_unique<Alloc>(BufferParam(name.c_str())); });

    // resetter is now paused after formatting, still holding l_shm_ex
    ASSERT_TRUE(g_points.wait_arrival("reset.formatted", std::chrono::seconds(5)));

    std::atomic<bool> b_done{false};
    std::thread tb([&] {
        b = std::make_unique<Alloc>(BufferParam(name.c_str()));
        b_done = true;
    });

    // b must be blocked behind the paused resetter, not constructing meanwhile
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(b_done.load()) << "attacher overtook a mid-format resetter";

    g_points.release("reset.formatted");
    ta.join();
    tb.join();

    ASSERT_NE(a->get_buf(), nullptr);
    ASSERT_NE(b->get_buf(), nullptr) << "attacher failed after resetter finished";
    EXPECT_EQ(read_slot(b->get_buf(), 0), MAGIC_NUMBER) << "attacher saw unformatted segment";
}

TEST_F(AllocatorTest, AttachDuringTeardownFailsClean) {
    auto a = std::make_unique<Alloc>(BufferParam(name.c_str()));
    ASSERT_NE(a->get_buf(), nullptr);

    g_points.block_at("free.reaping");
    std::thread td([&] { a.reset(); }); // pauses holding EX, right before unlink

    ASSERT_TRUE(g_points.wait_arrival("free.reaping", std::chrono::seconds(5)));

    std::unique_ptr<Alloc> b;
    std::atomic<bool> b_done{false};
    std::thread tb([&] {
        b = std::make_unique<Alloc>(BufferParam(name.c_str()));
        b_done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(b_done.load()) << "attacher ran while teardown held exclusivity";

    g_points.release("free.reaping");
    td.join();
    tb.join();

    // teardown defers to in-flight constructors (l_shm_ex) and constructors
    // are SH-protected before becoming visible, so b must NOT fail: it resets
    // the name the teardown just unlinked and gets a fresh segment
    ASSERT_NE(b->get_buf(), nullptr) << "attacher failed while racing a teardown";
    EXPECT_EQ(read_slot(b->get_buf(), 0), MAGIC_NUMBER);
    EXPECT_EQ(read_slot(b->get_buf(), 2), 0u) << "adopted stale head";
    EXPECT_EQ(read_slot(b->get_buf(), 3), 0u) << "adopted stale tail";
}

TEST_F(AllocatorTest, CrashedHolderIsReclaimed) {
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        // child: construct, then die without running destructors -- the kernel
        // releases the flocks but nobody unlinks, leaving a stale segment
        Alloc a{BufferParam(name.c_str())};
        _exit(a.get_buf() != nullptr ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child failed to allocate";
    ASSERT_TRUE(shm_name_exists(name)) << "segment should leak when holder dies";

    // next generation must reclaim and reformat it
    {
        Alloc a{BufferParam(name.c_str())};
        ASSERT_NE(a.get_buf(), nullptr) << "could not reclaim segment of dead holder";
        EXPECT_EQ(read_slot(a.get_buf(), 0), MAGIC_NUMBER);
        EXPECT_EQ(read_slot(a.get_buf(), 2), 0u);
    }
    EXPECT_FALSE(shm_name_exists(name)) << "segment not unlinked after reclaim";
}

} // namespace
