/*

  SPSC queue adapted from here:
  http://cbloomrants.blogspot.com/2009/02/02-26-09-low-level-threading-part-51.html

*/

#include <assert.h>
#include <iostream>
#include <thread>

template<typename T>
class spsc_queue_t
{
public:

    spsc_queue_t() :
        _head(reinterpret_cast<buffer_node_t*>(new buffer_node_aligned_t)),
        _tail(_head.load(std::memory_order_relaxed))
    {}

    ~spsc_queue_t()
    {
        T output;
        while (this->dequeue(output)) {}
        buffer_node_t* front = _head.load(std::memory_order_relaxed);
        delete front;
    }

    void
    enqueue(
        const T& input)
    {
        buffer_node_t* node = reinterpret_cast<buffer_node_t*>(new buffer_node_aligned_t);
        node->data = input;
        node->next.store(NULL, std::memory_order_relaxed);

        buffer_node_t* back = _tail.load(std::memory_order_relaxed);
        back->next.store(node, std::memory_order_release);
        _tail.store(node, std::memory_order_relaxed);
    }

    bool
    dequeue(
        T& output)
    {
        buffer_node_t* front = _head.load(std::memory_order_relaxed);
        buffer_node_t* next = front->next.load(std::memory_order_acquire);
        if (next == NULL) {
            // buffer is empty
            return false;
        }
        output = next->data;
        _head.store(next, std::memory_order_release);
        delete front;
        return true;
    }


private:

    struct buffer_node_t
    {
        buffer_node_t() :
            next(NULL)
        {}

        std::atomic<buffer_node_t*> next;
        T                           data;
    };

    typedef typename std::aligned_storage<sizeof(buffer_node_t), std::alignment_of<buffer_node_t>::value>::type buffer_node_aligned_t;


    std::atomic<buffer_node_t*> _head;
    std::atomic<buffer_node_t*> _tail;

    spsc_queue_t(const spsc_queue_t&) {}
    void operator=(const spsc_queue_t&) {}
};


int
main()
{
    spsc_queue_t<int>* buffer = new spsc_queue_t<int>();
    buffer->enqueue(0);
    buffer->enqueue(1);
    buffer->enqueue(2);
    buffer->enqueue(3);

    int output = -1;
    std::cout << "value 0: " << buffer->dequeue(output) << " " << output << std::endl;
    std::cout << "value 1: " << buffer->dequeue(output) << " " << output << std::endl;
    std::cout << "value 2: " << buffer->dequeue(output) << " " << output << std::endl;
    delete buffer;
}
