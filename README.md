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

// shm-backed (cross-process): first process to create becomes the owner and
// formats the buffer; later ones attach and wait for it to be ready
BufferAllocator<256, 1024, false> shm_alloc{BufferParam("/my-queue")};

SPSCBuffer<256> q(alloc.get_buf());          // get_buf() == nullptr on failure

PUSH_STATUS s = q.push(obj);                 // SUCCESS | FULL
BufData<256> d = q.pop();                    // d.rc: SUCCESS | EMPTY | OVERWRITTEN
```

The allocator owns the memory (RAII); the queue is a non-owning view over it.
Keep the allocator alive for the queue's whole lifetime.

### BufferParam

The shm name alone is enough — flags are handled for you:

```cpp
BufferParam(shm_name,                        // e.g. "/my-queue" (leading slash)
            shm_oflag  = O_RDWR | O_CREAT | O_EXCL,
            shm_perm   = 0600,
            mmap_prot  = PROT_READ | PROT_WRITE,
            mmap_flags = MAP_SHARED);
```

`O_CREAT | O_EXCL` is how the owner is elected (first creator wins, others
attach automatically), so if you override `shm_oflag`, those two flags must
stay set — the allocator rejects params without `O_EXCL`. The default
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

## Tests

```sh
cmake -B build -S .
cmake --build build
ctest --test-dir build
```

Includes single-threaded functional tests, randomized differential tests
against a reference model, and two-thread stress tests verifying ordering and
tear-freedom under contention.
