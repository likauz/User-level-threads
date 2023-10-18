/*
 * User-Level Threads Library (uthreads)
 * Hebrew University OS course.
 * Author: OS, os@cs.huji.ac.il
 */
#include<iostream>
#include <algorithm>
#include<list>
#include<deque>
#include<queue>
#include<vector>
#include<map>
#include <stdio.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include "uthreads.h"

using namespace std;
typedef unsigned long address_t;
typedef void (*thread_entry_point)(void);
enum Status {running, blocked, ready};


#define STACK_SIZE 4096 /* stack size per thread (in bytes) */
#define JB_SP 6
#define JB_PC 7


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}

/* External interface */
typedef struct thread {
    char stack[STACK_SIZE];
    Status status;
    sigjmp_buf env;
    bool is_sleeping = false;
    int quantum_counter = 0;
} thread;


map<int, thread*> threadList;
map<int, int> sleepThreads;
deque<int> queue_ready;
int running_thread;
bool run_and_terminate = false;
priority_queue<int, vector<int>, greater<int>> queue_tid;
struct itimerval timer;
int quantum_counter;


/**
 * active the mask signal
 */
void blocked_signal()
{
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGVTALRM);
    sigprocmask(SIG_BLOCK, &signal_set, nullptr);
}

/**
 * unactivated the mask signal
 */
void unblocked_signal()
{
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGVTALRM);
    sigprocmask(SIG_UNBLOCK, &signal_set, nullptr);
}

/**
 * a help function to wake all the sleep threads for a given quantum_counter
 */
void wake_sleep()
{
    if (!sleepThreads.empty())
    {
        std::map<int, int>::iterator itr;
        for(itr = sleepThreads.begin(); itr != sleepThreads.end(); ++itr)
        {
            if (itr->second == quantum_counter)
            {
                threadList[itr -> first] -> is_sleeping = false;
                uthread_resume(itr->first);
            }
        }
    }
}

/**
 * The switch function for threads.
 * it will automatically work in every signal SIGVTALRM raise,
 * or we will call it in thread's block or terminate
 * @param sig
 */
void timer_handler(int sig)
{
    blocked_signal();
    int old_tid = running_thread;
    int new_tid;

    wake_sleep();
    if(!queue_ready.empty())
    {
        if(!run_and_terminate)
        {
            if(sigsetjmp(threadList[old_tid] -> env, 1) != 0)
            {
                unblocked_signal();
                return;
            }
            if (threadList[old_tid] -> status != blocked)
            {
                threadList[old_tid] -> status = ready;
                queue_ready.push_back(old_tid);
            }
        }

        run_and_terminate = false;
        new_tid = queue_ready.front();
        queue_ready.pop_front();
        running_thread = new_tid;
        threadList[new_tid] -> status = running;
        threadList[new_tid] -> quantum_counter += 1;
        quantum_counter  += 1;
        if (setitimer(ITIMER_VIRTUAL, &timer, NULL))
        {
            printf("setitimer error.");
        }
        siglongjmp(threadList[running_thread] -> env, 1);
    }
    else
    {
        new_tid = running_thread;
        threadList[new_tid] -> quantum_counter += 1;
        quantum_counter  += 1;
        if (setitimer(ITIMER_VIRTUAL, &timer, NULL))
        {
            printf("setitimer error.");
        }
    }
    unblocked_signal();
}

/**
 * In order to initialize the threads time
 * @param quantum_usecs the cycle duration of our timer - quantum
 */
void set_timer(int quantum_usecs)
{
    blocked_signal();
    struct sigaction sa = {0};

    // Install timer_handler as the signal handler for SIGVTALRM.
    sa.sa_handler = &timer_handler;
    if (sigaction(SIGVTALRM, &sa, NULL) < 0)
    {
        std::cerr << "system error: sigaction error.\n";
        unblocked_signal();
        exit(1);
    }

    // Configure the my_timer to expire after 1 sec... */
    timer.it_value.tv_usec = quantum_usecs;        // first time interval, microseconds part

    // configure the my_timer to expire every 3 sec after that
    timer.it_interval.tv_usec = quantum_usecs;    // following time intervals, microseconds part

    // Start a virtual my_timer. It counts down whenever this process is executing.
    if (setitimer(ITIMER_VIRTUAL, &timer, NULL))
    {
        std::cerr << "thread library error: setitimer error.\n";
        unblocked_signal();
        exit(1);
    }
    unblocked_signal();
}


/**
 * @brief initializes the thread library.
 *
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs)
{
    blocked_signal();
    if(quantum_usecs <= 0)
    {
        std::cerr << "thread library error: invalid quantum_usecs\n";
        unblocked_signal();
        return -1;
    }

    for (int i=1; i<100; ++i)
    {
        queue_tid.push(i);
    }

    //thread
    thread* new_thread = new thread();
    new_thread -> status = running;
    new_thread -> quantum_counter += 1;
    quantum_counter += 1;
    running_thread = 0;
    threadList[0] = new_thread;

    // env
    sigsetjmp(new_thread -> env, 1);
    sigemptyset(&new_thread -> env->__saved_mask);

    set_timer(quantum_usecs);
    unblocked_signal();
    return 0;
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point)
{
    blocked_signal();
    if (queue_tid.empty())
    {
        std::cerr << "thread library error: adding new thread is invalid- we got to maximum threads number\n"; // todo check if system error
        unblocked_signal();
        return -1;
    }

    //thread
    int tid = queue_tid.top();
    queue_tid.pop();
    threadList[tid] = new thread();
    threadList[tid] -> status = ready;
    queue_ready.push_back(tid);

    // env
    address_t sp = (address_t) threadList[tid] -> stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t) entry_point;
    sigsetjmp(threadList[tid] -> env, 1);
    (threadList[tid] -> env->__jmpbuf)[JB_SP] = translate_address(sp);
    (threadList[tid] -> env->__jmpbuf)[JB_PC] = translate_address(pc);
    sigemptyset(&threadList[tid] -> env->__saved_mask);
    unblocked_signal();
    return tid;
}

/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate(int tid)
{
    //if tid doesn't exist we return an error
    blocked_signal();
    if(threadList.find(tid) == threadList.end())
    {
        std::cerr << "thread library error: no thread as given tid\n";
        unblocked_signal();
        return -1;
    }

    //if tid == 0 we need to delete everything
    std::map<int, thread*>::iterator itr;
    if(tid==0)
    {
        for(itr = threadList.begin(); itr != threadList.end(); ++itr)
        {
            delete itr->second;
        }
        threadList.clear();
        queue_ready.clear();
        unblocked_signal();
        exit(EXIT_SUCCESS);
    }

    //if tid !=  0
    else
    {
        std::deque<int>::iterator itr1;
        Status now_status = threadList[tid] -> status;
        queue_tid.push(tid); //insert the current tid to the queue_tid
        delete threadList[tid]; //delete thread
        threadList.erase(threadList.find(tid)); //delete thread from map
        switch(now_status)
        {
            case running:
                run_and_terminate = true;
                timer_handler(0);
                break;
            case ready:
                queue_ready.erase(std::find(queue_ready.begin(), queue_ready.end(), tid));
                break;
        }
    }
    unblocked_signal();
    return 0;
}


/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid)
{
    //if tid doesn't exist we return an error
    blocked_signal();
    if(threadList.find(tid) == threadList.end() || tid == 0)
    {
        std::cerr << "thread library error: invalid thread tid\n";
        unblocked_signal();
        return -1;
    }

    Status status = threadList[tid]-> status;
    threadList[tid]-> status = blocked;
    switch (status) {
        case blocked:
            unblocked_signal();
            return 0;
        case running:
            timer_handler(0);
            break;
        case ready:
            queue_ready.erase(std::find(queue_ready.begin(), queue_ready.end(), tid));
            break;
    }
    unblocked_signal();
    return 0;
}

/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid)
{
    blocked_signal();
    //if tid doesn't exist we return an error
    if(threadList.find(tid) == threadList.end())
    {
        std::cerr << "thread library error: invalid tid\n";
        unblocked_signal();
        return -1;
    }

    if (threadList[tid] -> is_sleeping)
    {
        unblocked_signal();
        return 0;
    }

    Status status = threadList[tid] -> status;
    if (status == blocked)
    {
        queue_ready.push_back(tid);
        threadList[tid] -> status = ready;
    }
    unblocked_signal();
    return 0;
}


/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY threads list.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid==0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums)
{
    blocked_signal();
    //if tid ==0 its error or num_quantums invalid
    if(running_thread == 0 || num_quantums <= 0)
    {
        std::cerr << "thread library error: invalid num_quantums\n";
        unblocked_signal();
        return -1;
    }
    threadList[running_thread] -> is_sleeping = true;
    sleepThreads[running_thread] = quantum_counter + num_quantums + 1;
    unblocked_signal();
    return uthread_block(running_thread);
}


/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid()
{
    return running_thread;
}


/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums()
{
    return quantum_counter;
}


/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums(int tid)
{
    blocked_signal();
    if(threadList.find(tid) == threadList.end())
    {
        std::cerr << "thread library error: invalid tid\n";
        return -1;
    }
    unblocked_signal();
    return threadList[tid] -> quantum_counter;
}






