#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace Code::Buffer {

template<typename Derived, typename T, bool Overwrite>
        requires std::is_trivially_copyable_v<T>
class BaseBuffer {

    static constexpr size_t atomic_alignment = std::atomic_ref<uint64_t>::required_alignment;
    using atomic_uint = std::atomic_ref<uint64_t>;
    static constexpr auto relaxed = std::memory_order_relaxed;
    static constexpr auto acquire = std::memory_order_acquire;
    static constexpr auto release = std::memory_order_release;

    protected:
        struct alignas(atomic_alignment) Slot {
            uint64_t data;
        };

        Slot* _slot_num_elements, *_slot_head, *_slot_tail;
        atomic_uint _head, _tail;
        std::vector<atomic_uint> _ver;

        // this is to construct a new Slot object without overwriting the underlying memory
        Slot* new_slot(uint8_t* buf) {
            if (buf == nullptr)
                throw std::invalid_argument("buf cannot be a null pointer");
            auto* ptr = new (buf) Slot;
            return ptr;
        }

        // initialize the version vector this only gets called if we are in overwrite mode
        void initialize_ver_array(uint8_t* buf, const uint64_t& num_elements) {
            _ver.reserve(num_elements);
            for (uint64_t i=0; i<num_elements; i++) {
                Slot* _tmp_slot = new_slot(buf + i * sizeof(Slot));
                _ver.emplace_back(_tmp_slot->data); 
            }
        }

        // check if the provided start pointer of buffer is atomically aligned
        uint8_t* assert_aligned(uint8_t* buf) {
            if(reinterpret_cast<std::uintptr_t>(buf) % atomic_alignment != 0) {
                throw std::logic_error("pointer provided in class is not atomically aligned");
            }
            return buf;
        }

    public:

        struct BufData {
            T data;
            uint8_t rc; // 0->success, 1->empty/no data, 2->data_overwritten
        };

        explicit BaseBuffer(uint8_t* buf)
            : _slot_num_elements(new_slot(assert_aligned(buf) + 1 * sizeof(Slot)))
            , _slot_head        (new_slot(buf + 2 * sizeof(Slot)))
            , _slot_tail        (new_slot(buf + 3 * sizeof(Slot)))
            , _head         (_slot_head->data)
            , _tail         (_slot_tail->data)
        {
            if(Overwrite)
            {
                initialize_ver_array(buf + 4 * sizeof(Slot), _slot_num_elements->data);
            }
        }

        BaseBuffer(const BaseBuffer&)            = delete;
        BaseBuffer& operator=(const BaseBuffer&) = delete;
        BaseBuffer(BaseBuffer&&)                 = delete;
        BaseBuffer& operator=(BaseBuffer&&)      = delete;

        ~BaseBuffer() {}

        bool is_empty() { return _head.load(relaxed) == _tail.load(relaxed); }
        bool is_full() { 
            return (_head.load(relaxed) - _tail.load(relaxed)) >= _slot_num_elements->data - 1; 
        }

        void push(const T& data) { static_cast<Derived*>(this)->_d_push(data); }        
        BufData pop() { return static_cast<Derived*>(this)->_d_pop(); }
};

}
