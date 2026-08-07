#pragma once

#include "base_buffer.hpp"
#include "common.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef GOOGLE_TEST
#endif

namespace Code::Buffer {

template<size_t MaxObjSize>
class SPSCBuffer : public BaseBuffer<SPSCBuffer<MaxObjSize>, false, MaxObjSize> {

    protected:
        using Base = BaseBuffer<SPSCBuffer<MaxObjSize>, false, MaxObjSize>;
        friend Base;

        template<typename T>
            requires PushableObject<T, MaxObjSize>
        PUSH_STATUS _d_push(const T& data) {
            auto head = this->get_head(relaxed);
            bool goto_top = true;
        top_p:
            auto tail = this->_c_tail;
            if ((head - tail) >= (this->_slot_num_elements->data - 1)) {
                if (goto_top) {
                    goto_top = false;
                    this->refresh_tail(acquire);
                    goto top_p;
                }
                return PUSH_STATUS::FULL;
            }

            memcpy(this->buf + (head & this->mask) * MaxObjSize, &data, sizeof(T));
            this->set_head(head + 1, release);

            return PUSH_STATUS::SUCCESS;
        }

        BufData<MaxObjSize> _d_pop() {
            auto tail = this->get_tail(relaxed);
            bool goto_top = true;
        top_c:
            auto head = this->_c_head;
            if (head == tail) {
                if (goto_top) {
                    goto_top = false;
                    this->refresh_head(acquire);
                    goto top_c;
                }
                return {{}, POP_STATUS::EMPTY};
            }

            BufData<MaxObjSize> data;
            memcpy(&(data.data), this->buf + (tail & this->mask) * MaxObjSize, MaxObjSize);
            data.rc = POP_STATUS::SUCCESS;
            this->set_tail(tail + 1, release);

            return data;
        }

        /*
         * ------------------------------------------------------------------------
         * MEMORY ORDERING RATIONALE (SPSC: one producer thread, one consumer thread)
         * ------------------------------------------------------------------------
         *
         * There are two ways to think about this, and both are used below:
         *
         * (A) The hardware view: a barrier is a one-directional ban on reordering.
         *     Every acquire/release guarantee is built from these four bans:
         *
         *          Load-Load   — a load  cannot be reordered after a later load
         *          Load-Store  — a load  cannot be reordered after a later store
         *          Store-Store — a store cannot be reordered after a later store
         *          Store-Load  — a store cannot be reordered after a later load
         *                        (the expensive one; only seq_cst provides it)
         *
         *          acquire (on a load)  => provides Load-Load  + Load-Store
         *                                  (nothing after the acquire-load may move
         *                                   before it)
         *          release (on a store) => provides Store-Store + Load-Store
         *                                  (nothing before the release-store may move
         *                                   after it)
         *
         * (B) The C++ standard view: synchronizes-with. A release-store on a variable
         *     "synchronizes-with" an acquire-load that later reads that SAME variable.
         *     Once that pairing holds, everything the releasing thread did before its
         *     store is guaranteed visible to the acquiring thread after its load
         *     (a happens-before edge is established across the two threads).
         *
         *     The two pairs in this queue:
         *          producer set_head(release)  <-->  consumer get_head(acquire)
         *              => makes the producer's payload write visible to the consumer
         *          consumer set_tail(release)  <-->  producer get_tail(acquire)
         *              => tells the producer the consumer has finished reading the slot
         *
         * (A) is HOW it manifests inside one thread; (B) is WHY it works across the
         * two threads. Each numbered point below is one half of one of those pairs.
         *
         * Reminder on who owns what: the producer is the ONLY writer of `head`, and
         * the consumer is the ONLY writer of `tail`. So a thread reading its OWN index
         * can use relaxed (no other thread writes it); reading the OTHER thread's index
         * needs acquire; publishing its own index needs release.
         *
         * ------------------------------------------------------------------------
         *
         * 1. Why release on the producer's store to head (after the memcpy write):
         *      The producer does two stores in order: (i) memcpy the payload INTO the
         *      slot, then (ii) store head+1. Both are STORES. Without release, store
         *      (ii) can be reordered before store (i), so the consumer could observe
         *      head advanced while the slot still holds old/partial data, and read it
         *      -> torn or stale read. Release keeps the payload store before the head
         *      store. (Store-Store)
         *
         * 2. Why release on the consumer's store to tail (after the memcpy read):
         *      The consumer does a LOAD (memcpy reading the slot) then a STORE (tail+1).
         *      Publishing tail hands the slot back to the producer to overwrite.
         *      Without release, the tail store can be reordered before the slot read
         *      finishes, so the producer sees the slot as free and overwrites it WHILE
         *      the consumer is still copying out of it -> data race / UB. Release keeps
         *      the slot read before the tail store. (Load-Store)
         *
         * 3. Why acquire on the consumer's load of head:
         *      The consumer does a LOAD of head, checks emptiness, then a LOAD of the
         *      slot (memcpy). Without acquire, the slot load can be hoisted ABOVE the
         *      head load. That means the consumer could read the slot before it has
         *      confirmed (via head) that the producer actually published data there,
         *      pulling out stale/garbage bytes from a slot that was logically empty.
         *      Acquire keeps the head load before the slot load, and (view B) pairs
         *      with the producer's head-release so the producer's payload write is
         *      actually visible once we see the advanced head. (Load-Load)
         *
         * 4. Why acquire on the producer's load of tail:
         *      The producer does a LOAD of tail (to check fullness) then a STORE (the
         *      memcpy write into the slot). Without acquire, the tail load can be
         *      reordered AFTER the memcpy write, so the producer effectively writes
         *      before it has checked whether the slot is free. Combined with (view B),
         *      the tail-acquire pairs with the consumer's tail-release, guaranteeing
         *      that when we observe an advanced tail the consumer has finished reading
         *      that slot -- so we never overwrite a slot mid-read. Acquire keeps the
         *      tail load before the payload store. (Load-Store)
         * */
    public:
        explicit SPSCBuffer(uint8_t* buf)
            : Base(buf) {}
};

template<size_t MaxObjSize>
class SPSCBufferOverwrite : public BaseBuffer<SPSCBufferOverwrite<MaxObjSize>, true, MaxObjSize> {

    protected:
        using Base = BaseBuffer<SPSCBufferOverwrite<MaxObjSize>, true, MaxObjSize>;
        friend Base;

        template<typename T>
            requires PushableObject<T, MaxObjSize>
        PUSH_STATUS _d_push(const T& data) {
            auto head = this->get_head(relaxed);
            this->set_head(head + 1, relaxed);
            this->_ver[head & this->mask].store(head * 2 + 1, relaxed);
            std::atomic_thread_fence(release);
            memcpy(this->buf + (head & this->mask) * MaxObjSize, &data, sizeof(T));
            this->_ver[head & this->mask].store(head * 2 + 2, release);
            return PUSH_STATUS::SUCCESS;
        }

        BufData<MaxObjSize> _d_pop() {
            auto tail = this->get_tail(relaxed);
            auto p_ver = this->_ver[tail & this->mask].load(acquire);

            if (p_ver <= 2 * tail + 1)
                return {{}, POP_STATUS::EMPTY};

            this->set_tail(tail + 1, relaxed);

            if ((p_ver & 1) || (p_ver > 2 * tail + 2)) {
                size_t p_head = (p_ver + 1) / 2 - 1;
                if (p_head > tail) {
                    // if tail has been overwritten then it is possible that head is
                    // very ahead so in that case we will move tail to `p_head-NumSlots+1`
                    this->set_tail(this->get_head(relaxed) - this->_slot_num_elements->data,
                                   relaxed);
                }
                return {{}, POP_STATUS::OVERWRITTEN};
            }

            BufData<MaxObjSize> data;
            memcpy(&(data.data), this->buf + (tail & this->mask) * MaxObjSize, MaxObjSize);
            data.rc = POP_STATUS::SUCCESS;
            std::atomic_thread_fence(acquire);
            if (p_ver != this->_ver[tail & this->mask].load(relaxed))
                data = {{}, POP_STATUS::OVERWRITTEN};

            return data;
        }
        /*
         * ------------------------------------------------------------------------
         * MEMORY ORDERING RATIONALE (OVERWRITE MODE: a per-slot seqlock)
         * ------------------------------------------------------------------------
         *
         * The protocol: each slot has a version counter. The producer stores
         * 2*seq+1 (ODD = write in progress), copies the payload, then stores
         * 2*seq+2 (EVEN = write complete). The consumer reads the version,
         * copies the payload out, then re-reads the version: if it changed,
         * the copy raced with a writer and is discarded. The version is both
         * the "is it ready" flag and the lap counter, so head/tail are never
         * used for cross-thread handoff here -- ALL synchronization goes
         * through the version. That is why every head/tail access in this
         * class is relaxed: head is written only by the producer and read
         * only by the producer; tail likewise for the consumer. They carry
         * no ordering duty at all (unlike the non-overwrite queue above).
         *
         * Four ordering edges make the seqlock sound. Two are expressed as
         * orderings ON the version operation itself, two MUST be standalone
         * fences. The rule that decides which is which:
         *
         *      If the required ordering points AT the atomic operation
         *      ("everything before X must be visible when X is seen") tag
         *      the operation itself with release/acquire -- cheapest.
         *      If the required ordering points ACROSS plain memory accesses
         *      ("this atomic store must precede those LATER plain stores",
         *      or "those EARLIER plain loads must precede this atomic
         *      load") no load/store flavor expresses it -- only a fence.
         *
         * 1. fence(release) between the ODD store and the payload memcpy
         *    (producer). Needed edge: odd-store BEFORE payload stores, so a
         *    concurrent reader that sees any new payload byte is guaranteed
         *    to (eventually, via its re-check) see the version bumped. A
         *    release STORE cannot do this: release orders what comes BEFORE
         *    the store, and here the odd store must be ordered against
         *    stores that come AFTER it. Wrong direction => standalone fence.
         *    (Store-Store)
         *
         * 2. release on the EVEN store (producer, no fence). Needed edge:
         *    payload stores BEFORE the completion store -- i.e. everything
         *    before X visible when X is seen. That points AT the store, so
         *    the ordering is attached to the one store that needs it instead
         *    of a fence that would constrain every later store. Pairs with
         *    the consumer's acquire load of the version (view B above).
         *    (Store-Store, but scoped to a single named store)
         *
         * 3. acquire on the version load (consumer, no fence). Needed edge:
         *    version load BEFORE payload loads, plus the synchronizes-with
         *    pairing against edge 2 so that seeing EVEN means the payload
         *    writes are actually visible. Points AT the load => tag the
         *    load. Bonus: the successful read path then contains NO fence
         *    at all -- on ARM this is one ldar instead of a dmb, and the
         *    data-ready-to-data-read latency is what matters most here.
         *    (Load-Load)
         *
         * 4. fence(acquire) between the payload memcpy and the version
         *    RE-load (consumer). Needed edge: payload loads BEFORE the
         *    re-check load, otherwise the re-check can be satisfied early
         *    and validate a torn copy. An acquire LOAD cannot do this:
         *    acquire orders what comes AFTER the load, and here the earlier
         *    plain loads must be ordered against a LATER atomic load. Wrong
         *    direction => standalone fence. Pairs with edge 1: if the copy
         *    overlapped a write, the odd store is guaranteed visible to the
         *    re-check, which then fails and discards the data. (Load-Load)
         *
         * Cost model (why we prefer tagging the operation over fencing):
         *      relaxed  <  acquire/release ON an op  <  standalone fence
         *      <  seq_cst (Store-Load, unused here)
         * A fence must order against EVERY past/future access because it
         * names no operation -- on ARM it compiles to dmb ish, which stalls
         * the whole out-of-order window and store buffer. A tagged op
         * (ldar/stlr) attaches the constraint to ONE instruction and lets
         * everything unrelated keep flowing. On x86 (TSO) all of these
         * compile to plain mov (fences become compiler-only barriers), so
         * the distinction only costs on weakly-ordered hardware. The two
         * fences that remain (edges 1 and 4) are exactly the two edges a
         * tagged operation cannot express -- and note where they sit: edge 1
         * on the write path, edge 4 AFTER the consumer's copy. The hot
         * empty-poll path and the load side of a successful pop never
         * execute a fence at all.
         *
         * Caveat: the two memcpys can touch the same slot concurrently --
         * formally a data race on non-atomic bytes (the re-check discards
         * the result, but the C++ standard doesn't bless the read itself).
         * Every practical seqlock (Linux kernel included) accepts this; it
         * is correct on real x86/ARM hardware. Do not "fix" it by making
         * the copy atomic word-by-word without measuring first.
         * */

    public:
        explicit SPSCBufferOverwrite(uint8_t* buf)
            : Base(buf) {}
};

} // namespace Code::Buffer
