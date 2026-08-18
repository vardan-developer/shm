# shm

Header-only, lock-free SPSC (single-producer single-consumer) ring buffers for
C++20, backed by heap memory or POSIX shared memory for cross-process use.

Drop-in: copy the headers from `include/` into your project's include path —
there is nothing to build or link. A C port is under way.

## Buffers

- **`SPSCBuffer<MaxObjSize>`** — bounded, non-overwrite. `push` fails with
  `FULL` when the ring is full; nothing is ever lost.
- **`SPSCBufferOverwrite<MaxObjSize>`** — overwrite mode (per-slot seqlock).
  `push` always succeeds and overwrites the oldest entry; a lapped consumer
  gets `OVERWRITTEN` and resynchronizes to the oldest live entry.

## API

```cpp
using namespace Code::Buffer;

// heap-backed
BufferAllocator<256 /*MaxObjSize*/, 1024 /*NumSlots*/, false /*Overwrite*/> alloc{};

// shm-backed (cross-process): first arrival formats the segment, the rest
// attach to it once it is ready
BufferAllocator<256, 1024, false> shm_alloc{BufferParam("/my-queue")};

SPSCBuffer<256> q(alloc.get_buf());          // get_buf() == nullptr on failure

PUSH_STATUS s = q.push(obj);                 // SUCCESS | FULL
BufData<256> d = q.pop();                    // d.rc: SUCCESS | EMPTY | OVERWRITTEN
```

### Allocator

The allocator owns the memory (RAII); the queue is a non-owning view over it.
Keep the allocator alive for the queue's whole lifetime.

For shm it also manages the segment lifecycle, coordinated through two lock
files in `/tmp` derived from the shm name:

- Constructors serialize on an exclusive lock. The first arrival that finds no
  live users unlinks any stale segment and creates and formats a fresh one;
  everyone else attaches to the ready segment. No polling, no handshake.
- Every attached allocator holds a shared lock for its lifetime — the liveness
  refcount. The last destructor out unlinks the segment.
- Locks are kernel-managed (`flock`), so a crashed process releases its hold
  automatically; the leftover segment is reclaimed and reformatted by the next
  construction.

### BufferParam

The shm name alone is enough — flags are handled for you:

```cpp
BufferParam(shm_name,                        // e.g. "/my-queue" (leading slash)
            shm_oflag  = O_RDWR | O_CREAT,
            shm_perm   = 0600,
            mmap_prot  = PROT_READ | PROT_WRITE,
            mmap_flags = MAP_SHARED);
```

Creation flags are managed by the allocator itself — it adds `O_CREAT | O_EXCL`
when resetting a segment and strips them when attaching — so overriding
`shm_oflag` is rarely needed. The name must start with `/`. The default
constructor `BufferParam()` selects heap mode instead.

## Constraints

- Exactly one producer thread/process and one consumer thread/process.
- `NumSlots` must be a power of two. Non-overwrite mode stores `NumSlots - 1`
  elements.
- Pushed types must be trivially copyable with `sizeof(T) <= MaxObjSize`
  (enforced by concept).
- Shared memory is same-host only; all processes must use identical template
  parameters for the same buffer.

## Guarantees

- **Non-overwrite:** every message is delivered exactly once, in FIFO order,
  untorn.
- **Overwrite:** `push` never blocks or fails. Delivered messages are untorn
  and strictly in order; messages may be lost to overwrite, but loss is always
  announced via `OVERWRITTEN` — never silent.
- Lock-free, syscall-free hot paths. The memory-ordering rationale for every
  atomic and fence is documented in `include/spsc_buffer.hpp`.
- **Allocator (shm):** a segment is only formatted while it has no attached
  users, and only unlinked by its last user. Constructions and teardowns may
  race freely across processes — teardowns defer to in-flight constructors, so
  construction never fails spuriously. Worst case after a crash or race is a
  leaked segment, which the next construction reclaims.

## Tests

```sh
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

Includes single-threaded functional tests, randomized differential tests
against a reference model, two-thread stress tests verifying ordering and
tear-freedom under contention, and allocator lifecycle tests that use
compiled-in pause points to force specific cross-thread interleavings
(concurrent construction, teardown races, crash recovery via fork).
