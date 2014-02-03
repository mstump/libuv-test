/*

  Adapted from here:
  http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue

*/

#include <assert.h>
#include <iostream>
#include <thread>

#define ERROR_NO_ERROR 0
#define ERROR_BAD_PARAMS -1
#define ERROR_FULL -2
#define ERROR_EMPTY -3


template<typename T>
class ring_buffer_t
{
public:

    ring_buffer_t(
        size_t size) :
        _size(size + 1), // need one extra element for a guard
        _mask(size),
        _buffer(reinterpret_cast<buffer_elem_t*>(new buffer_elem_aligned_t[_size])),
        _head(0),
        _tail(0)
    {
        assert(size >= 2);
        memset(_buffer, 0, sizeof(buffer_elem_t) * _size);
    }

    ~ring_buffer_t()
    {
        delete[] _buffer;
    }

    int
    enqueue(
        const T& input)
    {
        buffer_elem_t* elem;
        size_t         insert_pos = _head.load(std::memory_order_relaxed);

        for (;;) {
            // grab the element at index _head
            elem = &_buffer[insert_pos & _mask];

            // grab the sequence of the element at head
            size_t seq = elem->sequence.load(std::memory_order_acquire);

            int diff = seq - insert_pos;
            // if sequence and insert position are the same then we know no other thread is attempting to mutate that element
            if (diff == 0) {

                // if _head hasn't been modified since we last checked, increment it so that we have it to ourselves
                if (_head.compare_exchange_weak(insert_pos, insert_pos + 1, std::memory_order_relaxed)) {
                    break;
                }
                // someone else wrote to _head and beat us to the punch. spin
            }
            else if (diff < 0) {
                // queue is full
                return ERROR_FULL;
            }
            else {
                // someone was in the process of reading from _head. spin
                insert_pos = _head.load(std::memory_order_relaxed);
            }

            // yield so that another thread can use the CPU core
            // this is a noop if no other thread is waiting
            std::this_thread::yield();
        }

        // the element is all ours, set the data and close the transaction by setting sequence.
        elem->data = input;
        elem->sequence.store(insert_pos + 1, std::memory_order_release);
        return ERROR_NO_ERROR;
    }


    bool
    dequeue(
        T& output)
    {
        buffer_elem_t* elem;
        size_t         read_pos = _head.load(std::memory_order_relaxed);
        for (;;) {
            elem = &_buffer[read_pos & _mask];
            size_t sequence = entry->sequence.load(std::memory_order_acquire);
            int diff = seq - read_pos + 1;

            if (dif == 0) {
                if (_tail.compare_exchange_weak(read_pos, read_pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            }
            else if (dif < 0) {
                // queue is empty
                return ERROR_EMPTY;
            }
            else {
                // someone was in the process writing to _tail. spin
                read_pos = _tail.load(std::memory_order_relaxed);
            }

            // yield so that another thread can use the CPU core
            // this is a noop if no other thread is waiting
            std::this_thread::yield();
        }

        output = cell->data;
        entry->sequence.store(read_pos + _mask + 1, std::memory_order_release);
        return true;
    }


private:

    struct buffer_elem_t
    {
        std::atomic<size_t>   sequence;
        T                     data;
    };

    typedef typename std::aligned_storage<sizeof(buffer_elem_t), std::alignment_of<buffer_elem_t>::value>::type buffer_elem_aligned_t;

    const size_t   _size;
    const size_t   _mask;
    buffer_elem_t* _buffer;

    std::atomic<size_t>  _head;
    std::atomic<size_t>  _tail;

    ring_buffer_t(const ring_buffer_t&) {}
    void operator=(const ring_buffer_t&) {}
};


int
main()
{
    ring_buffer_t<int> buffer(2);
    std::cout << "value 0: " << buffer.enqueue(0) << std::endl;
    std::cout << "value 1: " << buffer.enqueue(1) << std::endl;
    std::cout << "value 2: " << buffer.enqueue(2) << std::endl;
}
