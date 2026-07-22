#pragma once

#include "base_buffer.hpp"
#include <cstdint>
#include <cstring>

namespace Code::Buffer {

template<size_t MaxObjSize>
class SPSCBuffer : public BaseBuffer<SPSCBuffer<MaxObjSize>, false, MaxObjSize> {

    protected:

        using Base = BaseBuffer<SPSCBuffer<MaxObjSize>, false, MaxObjSize>;
        friend Base;
        
        template<typename T>
            requires PushableObject<T, MaxObjSize> 
            bool _d_push(const T& data) {
                auto head = this->get_head(relaxed);
                bool goto_top = true;
            top_p:
                auto tail = this->_c_tail;
                if((head - tail) >= (this->_slot_num_elements->data - 1)) { 
                    if(goto_top) {
                        goto_top = false;
                        this->refresh_tail(acquire);
                        goto top_p;
                    }
                    return false; 
                }  

                memcpy(this->buf + (head & this->mask) * MaxObjSize, &data, sizeof(T));
                this->set_head(head+1, release);

                return true;
            }

        BufData<MaxObjSize> _d_pop() {
            auto tail = this->get_tail(relaxed);
            bool goto_top = true;
        top_c:
            auto head = this->_c_head;
            if(head == tail){
                if (goto_top) {
                    goto_top = false;
                    this->refresh_head(acquire); 
                    goto top_c;
                }
                return {{}, 1};
            }

            BufData<MaxObjSize> data;
            memcpy(&(data.data), this->buf + (tail & this->mask) * MaxObjSize, MaxObjSize);
            data.rc = 0;
            this->set_tail(tail+1, release);

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

        explicit SPSCBuffer(uint8_t* buf) : Base(buf) {}
};

}
