/*

  SPSC queue adapted from here:
  http://cbloomrants.blogspot.com/2009/02/02-26-09-low-level-threading-part-51.html

*/

#include <assert.h>
#include <iostream>
#include <thread>

#include <uv.h>

template<typename T>
class spsc_queue_t
{
public:

    spsc_queue_t() :
        _head(reinterpret_cast<buffer_node_t*>(new buffer_node_aligned_t)),
        _tail(_head.load(std::memory_order_relaxed))
    {
        buffer_node_t* front = _head.load(std::memory_order_relaxed);
        front->next.store(NULL, std::memory_order_relaxed);
    }

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
        std::atomic<buffer_node_t*> next;
        T                           data;
    };

    typedef typename std::aligned_storage<sizeof(buffer_node_t), std::alignment_of<buffer_node_t>::value>::type buffer_node_aligned_t;

    std::atomic<buffer_node_t*> _head;
    std::atomic<buffer_node_t*> _tail;

    spsc_queue_t(const spsc_queue_t&) {}
    void operator=(const spsc_queue_t&) {}
};


template<typename T>
class mpsc_queue_t
{
public:

    mpsc_queue_t() :
        _head(reinterpret_cast<buffer_node_t*>(new buffer_node_aligned_t)),
        _tail(_head.load(std::memory_order_relaxed))
    {
        buffer_node_t* front = _head.load(std::memory_order_relaxed);
        front->next.store(NULL, std::memory_order_relaxed);
    }

    ~mpsc_queue_t()
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

        buffer_node_t* prev_head = _head.exchange(node, std::memory_order_acq_rel);
        prev_head->next.store(node, std::memory_order_release);
    }

    bool
    dequeue(
        T& output)
    {
        buffer_node_t* tail = _tail.load(std::memory_order_relaxed);
        buffer_node_t* next = tail->next.load(std::memory_order_acquire);

        if (next == NULL) {
            // buffer is empty
            return false;
        }

        output = next->data;
        _tail.store(next, std::memory_order_release);
        delete tail;
        return true;
    }


private:

    struct buffer_node_t
    {
        std::atomic<buffer_node_t*> next;
        T                           data;
    };

    typedef typename std::aligned_storage<sizeof(buffer_node_t), std::alignment_of<buffer_node_t>::value>::type buffer_node_aligned_t;

    std::atomic<buffer_node_t*> _head;
    std::atomic<buffer_node_t*> _tail;

    mpsc_queue_t(const mpsc_queue_t&) {}
    void operator=(const mpsc_queue_t&) {}
};


#define ERROR_NO_ERROR 0
#define IS_ERROR(e) e != ERROR_NO_ERROR

template<typename Work,
         typename Result>
struct request_t {

    typedef std::function<void(request_t<Work,Result>*)> callable_t;

    std::atomic<bool>       flag;
    std::mutex              mutex;
    std::condition_variable condition;
    Work                    work;
    Result                  result;
    int                     error;
    callable_t              callback;
    uv_work_t               uv_work_req;

    request_t() :
        flag(false),
        error(ERROR_NO_ERROR),
        callback(NULL)
    {}

    bool
    ready()
    {
        return flag.load(std::memory_order_consume);
    }

    void
    notify(
        uv_loop_t* loop)
    {
        flag.store(true, std::memory_order_release);
        condition.notify_all();

        if (callback) {
            uv_work_req.data = this;
            uv_queue_work(loop, &uv_work_req, &request_t<Work, Result>::callback_executor, NULL);
        }
    }

    void
    wait()
    {
        if (!flag.load(std::memory_order_consume)) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, std::bind(&request_t<Work, Result>::ready, this));
        }
    }

    template< class Rep, class Period >
    bool
    wait_for(
        const std::chrono::duration<Rep, Period>& time)
    {
        if (!flag.load(std::memory_order_consume)) {
            std::unique_lock<std::mutex> lock(mutex);
            return condition.wait_for(lock, time, std::bind(&request_t<Work, Result>::ready, this));
        }
    }

    static void
    callback_executor(
        uv_work_t* work)
    {
        (void) work;
        if (work && work->data) {
            request_t<Work,Result>* request = (request_t<Work,Result>*) work->data;
            if (request->callback) {
                request->callback(request);
            }
        }
    }

private:
    request_t(request_t&) {}
    void operator=(const request_t&) {}
};


struct context_t {
    typedef request_t<int, int>     caller_request_t;
    mpsc_queue_t<caller_request_t*> caller_input;
    uv_async_t                      caller_input_available;
    uv_loop_t*                      worker_loop;
    uv_thread_t                     worker_thread;


    context_t() :
        worker_loop(uv_loop_new())
    {
        uv_async_init(worker_loop, &caller_input_available, context_t::worker_consume_input_handle);
        uv_thread_create(&worker_thread, context_t::run_loop, worker_loop);
    }

    ~context_t()
    {
        uv_stop(worker_loop);
    }

    void
    init()
    {}

    caller_request_t*
    create_request(
        int data)
    {
        return create_request(data, NULL);
    }

    caller_request_t*
    create_request(
        int                          data,
        caller_request_t::callable_t callback)
    {
        caller_request_t* request = new caller_request_t();
        request->work = data;
        request->callback = callback;

        caller_input.enqueue(request);
        caller_input_available.data = (void*) this;
        uv_async_send(&caller_input_available);
        return request;
    }


private:
    static void
    run_loop(
        void* loop)
    {
        uv_run((uv_loop_t*) loop, UV_RUN_DEFAULT);
    }

    static void
    worker_consume_input_handle(
        uv_async_t* handle,
        int)
    {
        context_t* context = (context_t*) handle->data;
        std::cout << "worker signaled" << std::endl;

        caller_request_t* request;
        while (context->caller_input.dequeue(request)) {
            request->result = request->work;
            std::cout << "worker received request '" << request->work << "' responding with '" << request->result << "'" << std::endl;
            request->notify(context->worker_loop);
        }
    }

};

#include <thread>

std::atomic<int> COUNT(100);

void
my_callback(
    context_t::caller_request_t* request)
{
    delete request;
    COUNT.fetch_sub(1);
}

int
main()
{
    context_t c;
    c.init();

    for (int i = COUNT.load(std::memory_order_relaxed); i > 0; i--) {
        c.create_request(i, my_callback);
    }

    for (;;) {
        int count = COUNT.load(std::memory_order_consume);
        if (! count > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
