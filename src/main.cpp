// This is free and unencumbered software released into the public domain.

// Anyone is free to copy, modify, publish, use, compile, sell, or
// distribute this software, either in source code form or as a compiled
// binary, for any purpose, commercial or non-commercial, and by any
// means.

// In jurisdictions that recognize copyright laws, the author or authors
// of this software dedicate any and all copyright interest in the
// software to the public domain. We make this dedication for the benefit
// of the public at large and to the detriment of our heirs and
// successors. We intend this dedication to be an overt act of
// relinquishment in perpetuity of all present and future rights to this
// software under copyright law.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

// For more information, please refer to <http://unlicense.org/>

#include <assert.h>
#include <iostream>
#include <thread>
#include <uv.h>

// ERROR Codes
#define ERROR_NO_ERROR 0
#define IS_ERROR(e) e != ERROR_NO_ERROR


// non-intrusive lock free unbounded SPSC queue
// http://cbloomrants.blogspot.com/2009/02/02-26-09-low-level-threading-part-51.html

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

// non-intrusive lock free unbounded MPSC queue
// http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue

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

    /**
     * call to set the ready condition to true and notify interested parties
     * threads blocking on wait or wait_for will be woken up and resumed
     * if the caller specified a callback it will be triggered
     *
     * be sure to set the value of result prior to calling notify
     *
     * we execute the callback in a separate thread so that badly
     * behaving client code can't interfere with event/network handling
     *
     * @param loop the libuv event loop
     */
    void
    notify(
        uv_loop_t* loop)
    {
        flag.store(true, std::memory_order_release);
        condition.notify_all();

        if (callback) {
            // we execute the callback in a separate thread so that badly
            // behaving client code can't interfere with event/network handling
            uv_work_req.data = this;
            uv_queue_work(loop, &uv_work_req, &request_t<Work, Result>::callback_executor, NULL);
        }
    }

    /**
     * wait until the ready condition is met and results are ready
     */
    void
    wait()
    {
        if (!flag.load(std::memory_order_consume)) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, std::bind(&request_t<Work, Result>::ready, this));
        }
    }

    /**
     * wait until the ready condition is met, or specified time has elapsed.
     * return value indicates false for timeout
     *
     * @param time
     *
     * @return false for timeout, true if value is ready
     */
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

private:
    /**
     * Called by the libuv worker thread, and this method calls the callback
     * this is done to isolate customer code from ours
     *
     * @param work
     */
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

    // don't allow copy
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

    /**
       does the work of actually running the event loop
       this is called from a dedicated thread
    */
    static void
    run_loop(
        void* loop)
    {
        uv_run((uv_loop_t*) loop, UV_RUN_DEFAULT);
    }

    /**
       called by the event loop when an event is triggered
       signaling that there are items in the request queue
    */
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

    // don't allow copy
    context_t(context_t&) {}
    void operator=(const context_t&) {}
};

//////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  this code is to demonstrate how to expose C++ classes via opaque C structures and C functions
//
//  this methodology provides several benefits: eliminates the C++ ABI problem, prevents
//  implementation details from leaking into customer code, and allows us to modify implementation
//  details with forcing customers to recompile.
//
//  If you don't want to constantly cast between void* and the implementation types you can define
//  an incomplete type in the headers exposed to the customer.
//
//////////////////////////////////////////////////////////////////////////////////////////////////////

typedef void (*callback_t)(void*);

void*
new_context()
{
    return new context_t();
}

void
free_context(
    void* context)
{
    delete (context_t*) context;
}

void*
context_create_request(
    void* context,
    int   work)
{
    return ((context_t*) context)->create_request(work);
}

void
context_create_request_callback(
    void*      context,
    int        work,
    callback_t callback)
{
    ((context_t*) context)->create_request(work, (context_t::caller_request_t::callable_t)callback);
}

void
free_request(
    void* request)
{
    delete (context_t::caller_request_t*) request;
}

bool
request_ready(
    void* request)
{
    return ((context_t::caller_request_t*) request)->ready();
}

void
request_wait(
    void* request)
{
    ((context_t::caller_request_t*) request)->wait();
}

int
request_result(
    void* request)
{
    return ((context_t::caller_request_t*) request)->result;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  MAIN FUNCTION AND DEMO CODE
//
//////////////////////////////////////////////////////////////////////////////////////////////////////



// a global atomic so we can keep track of how many times the callback was called.
std::atomic<int> COUNT(10);

void
my_callback(
    context_t::caller_request_t* request)
{
    // when using the callback it's the job of the callback to delete
    // the request freeing up all memory allocated for that request
    // this is done because the library doesn't know how long the caller
    // will need to keep the request around
    delete request;
    COUNT.fetch_sub(1);
}

int
main()
{
    {
        // create the context
        context_t c;

        std::cout << "//////////////////////////////////////////////////////////////////////" << std::endl;
        std::cout << "//" << std::endl;
        std::cout << "//               STARTING CALLBACKS DEMO" << std::endl;
        std::cout << "//" << std::endl;
        std::cout << "//////////////////////////////////////////////////////////////////////" << std::endl;
        std::cout << std::endl;

        // queue up a bunch of requests
        for (int i = COUNT.load(std::memory_order_relaxed); i > 0; i--) {
            c.create_request(i, my_callback);
        }

        // wait until we've received callbacks from all of the requests
        for (;;) {
            int count = COUNT.load(std::memory_order_consume);
            if (! count > 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << std::endl << std::endl << std::endl;
        std::cout << "//////////////////////////////////////////////////////////////////////" << std::endl;
        std::cout << "//" << std::endl;
        std::cout << "//               STARTING FUTURE/PROMISE DEMO" << std::endl;
        std::cout << "//" << std::endl;
        std::cout << "//////////////////////////////////////////////////////////////////////" << std::endl;
        std::cout << std::endl;

        for (int i = 0; i < 10; ++i) {
            context_t::caller_request_t* request = c.create_request(i);
            request->wait();
            std::cout << "future/promise returned from the wait condition";
            std::cout << ", future/promise is ready: " << (request->ready() ? "TRUE" : "FALSE");
            std::cout << ", future/promise result is: " << request->result << std::endl;

            // when using the future/promise it's the job of the caller to delete
            // the request freeing up all memory allocated for that request.
            // this is done because the library doesn't know how long the caller
            // will need to keep the request around
            delete request;
        }
    }

    {
        std::cout << std::endl << std::endl << std::endl;
        std::cout << "//////////////////////////////////////////////////////////////////////" << std::endl;
        std::cout << "//" << std::endl;
        std::cout << "//            STARTING OPAQUE STRUCTURES/POINTERS DEMO" << std::endl;
        std::cout << "//" << std::endl;
        std::cout << "//////////////////////////////////////////////////////////////////////" << std::endl;
        std::cout << std::endl;

        void* c = new_context();

        for (int i = 0; i < 10; ++i) {
            void* request = context_create_request(c, i);
            request_wait(request);
            std::cout << "future/promise returned from the wait condition";
            std::cout << ", future/promise is ready: " << (request_ready(request) ? "TRUE" : "FALSE");
            std::cout << ", future/promise result is: " << request_result(request) << std::endl;

            // when using the future/promise it's the job of the caller to delete
            // the request freeing up all memory allocated for that request.
            // this is done because the library doesn't know how long the caller
            // will need to keep the request around
            free_request(request);
        }
        free_context(c);
    }
}
