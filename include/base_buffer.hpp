#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <new>

namespace Code::Buffer {

template<typename T, size_t MaxObjSize>
concept PushableObject = std::is_trivially_copyable_v<T> && (sizeof(T) <= MaxObjSize);

inline constexpr auto relaxed = std::memory_order_relaxed;
inline constexpr auto acquire = std::memory_order_acquire;
inline constexpr auto release = std::memory_order_release;
inline constexpr size_t atomic_alignment = std::atomic_ref<uint64_t>::required_alignment;

#ifndef  __cpp_lib_hardware_interference_size
inline constexpr auto cache_line_size = 64;
#else
inline constexpr auto cache_line_size = std::hardware_destructive_interference_size;
#endif

using atomic_uint = std::atomic_ref<uint64_t>;
struct alignas(cache_line_size) Slot {
    uint64_t data;
};
static_assert(alignof(Slot) % atomic_alignment == 0);

template<size_t MaxObjSize>
struct BufData {
    uint8_t data[MaxObjSize];
    uint8_t rc;
};


template<typename Derived, bool Overwrite, size_t MaxObjSize>
class BaseBuffer {

    protected:
        Slot* _slot_num_elements, *_slot_head, *_slot_tail;
        atomic_uint _head, _tail;
        std::vector<atomic_uint> _ver;
        uint8_t* buf;
        uint64_t mask;
        alignas(cache_line_size) uint64_t _c_head;
        alignas(cache_line_size) uint64_t _c_tail;

        // this is to construct a new Slot object without overwriting the underlying memory
        static Slot* new_slot(uint8_t* buf) {
            if (buf == nullptr)
                throw std::invalid_argument("buf cannot be a null pointer");
            auto* ptr = new (buf) Slot;
            return ptr;
        }

        // check if the provided start pointer of buffer is atomically aligned
        static uint8_t* require_aligned(uint8_t* buf) {
            if(reinterpret_cast<std::uintptr_t>(buf) % atomic_alignment != 0) {
                throw std::logic_error("pointer provided in class is not atomically aligned");
            }
            return buf;
        }

        static bool isPowerOfTwo(uint64_t n) {
            return n != 0 && (n & (n - 1)) == 0;
        }

        // initialize the version vector this only gets called if we are in overwrite mode
        void initialize_ver_array(uint8_t* buf, const uint64_t& num_elements) {
            _ver.reserve(num_elements);
            for (uint64_t i=0; i<num_elements; i++) {
                Slot* _tmp_slot = new_slot(buf + i * sizeof(Slot));
                _ver.emplace_back(_tmp_slot->data); 
            }
        }

        uint64_t get_head(const std::memory_order order) const { return _head.load(order); }
        void set_head(uint64_t val, const std::memory_order order) { 
            _head.store(val, order);
        }
        uint64_t get_tail(const std::memory_order order) const { return _tail.load(order); }
        void set_tail(uint64_t val, const std::memory_order order) {
            _tail.store(val, order);
        }

    public:
        explicit BaseBuffer(uint8_t* buf)
            : _slot_num_elements(new_slot(require_aligned(buf) + 1 * sizeof(Slot)))
            , _slot_head        (new_slot(buf + 2 * sizeof(Slot)))
            , _slot_tail        (new_slot(buf + 3 * sizeof(Slot)))
            , _head         (_slot_head->data)
            , _tail         (_slot_tail->data)
            , mask(_slot_num_elements->data - 1)
            , _c_head(_slot_head->data)
            , _c_tail(_slot_tail->data)
        {
            if (!isPowerOfTwo(_slot_num_elements->data)) {
                throw std::invalid_argument("the number of elements in the queue must be a power of 2");
            }
            size_t offset = 4 * sizeof(Slot);

            if(Overwrite)
            {
                initialize_ver_array(buf + offset, _slot_num_elements->data);
                offset += _slot_num_elements->data * sizeof(Slot);
            }

            this->buf = buf + offset;
        }

        BaseBuffer(const BaseBuffer&)            = delete;
        BaseBuffer& operator=(const BaseBuffer&) = delete;
        BaseBuffer(BaseBuffer&&)                 = delete;
        BaseBuffer& operator=(BaseBuffer&&)      = delete;

        ~BaseBuffer() {}

        bool is_empty() const { return _head.load(relaxed) == _tail.load(relaxed); }
        bool is_full() const {
            return (_head.load(relaxed) - _tail.load(relaxed)) >= _slot_num_elements->data - 1; 
        }

        void refresh_head(const std::memory_order order) { _c_head = _head.load(order); }
        void refresh_tail(const std::memory_order order) { _c_tail = _tail.load(order); }

        // returns true on a successful push false if the push failed
        template<typename T>
            requires PushableObject<T, MaxObjSize> 
        bool push(const T& data) { return static_cast<Derived*>(this)->_d_push(data); }

        BufData<MaxObjSize> pop() { return static_cast<Derived*>(this)->_d_pop(); }
};

}
