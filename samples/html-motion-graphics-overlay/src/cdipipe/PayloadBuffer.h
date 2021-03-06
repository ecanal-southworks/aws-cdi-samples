#pragma once

#include <mutex>
#include <boost/circular_buffer.hpp>

#include "Payload.h"

namespace CdiTools
{
    class PayloadBuffer
    {
    public:
        PayloadBuffer(size_t buffer_capacity);

        bool enqueue(const Payload& item);
        Payload front();
        void pop_front();
        void clear();
        size_t size();
        bool is_full();
        bool is_empty();
        inline size_t capacity() const { return buffer_.capacity(); }

    private:
        std::mutex gate_;
        boost::circular_buffer<Payload> buffer_;
    };
}
