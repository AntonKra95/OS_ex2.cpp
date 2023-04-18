//
// Created by Admin on 08/04/2023.
//

#include <iostream>
#include <algorithm>
#include <vector>
#include <sys/time.h>
#include <signal.h>
#include "uthreads.h"
#include <setjmp.h>
#include <map>

#define SUCCESS 0
#define FAILURE -1
#define RUNNING 0
#define BLOCKED 1
#define READY 2
#define FIRST_ID 0
#define JB_SP 6
#define JB_PC 7
#define ZERO 0;
#define UNBLOCKED 0;
#define DEFAULT_VALUE 1

#define SYSTEM_ERROR "system error: "
#define SETITIMER_ERROR "setitimer error"
#define SIGACTION_ERROR "sigaction error"
#define SIGEMPTYSET_ERROR "sigemptyset error"
#define SIGADDSET_ERROR "sigaddset error"
#define SIGPENDING_ERROR "sigpending error"
#define THREAD_LIBRARY_ERROR "thread library error: "
#define NON_POSITIVE_QUANTUM_USECS "given non-positive quantum_usecs"
#define EXCEED_THE_LIMIT "the number of current threads exceeds the limit"
#define NO_TID "no thread with ID tid exists"
#define BLOCKING_MAIN_THREAD "Tried blocking the main thread"
#define SLEEP_MAIN_THREAD "Tried sleeping the main thread"
#define NULL_ENTRY_POINT "given nullptr entry point"

#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
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

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
    : "=g" (ret)
    : "0" (addr));
    return ret;
}

#endif

/**
 * A struct for a thread
 * stage: 0-running, 1-blocked, 2-ready
 */
typedef struct thread
{
    int quantum_counter, id, stage;
    address_t sp, pc;
    char * stack;
    sigjmp_buf data;
} thread;

// static vars declaration:
static thread * threads[MAX_THREAD_NUM];  // an array of pointers to thread
static std::vector<thread*>  ready_threads;  // represents the ready threads queue

// a data struct for the sleeping threads. first val = quantoms to wait, second val = is blocked or not
std::map<int, std::pair<int, int>>sleeping_threads;

static int quantums_secs;
static thread *running_thread = nullptr;  // a pointer to the current running thread
static int total_quantums;  // represents the total amount of quantums that have been done
struct itimerval timer; // timer struct
static sigset_t mask;  // a data structure to hold the blocked signals
static sigset_t pending_signals;  // a data structure to hold pending signals
static bool execute_timer;

static void timer_init(int quantum_usecs); // todo

// Static function implementation

/**
 * A function that gets a thread id and deletes it
 * the function assumes that the given tid exists
 * @param tid represents the given thread id
 */
static void free_thread (int tid)
{
  delete [] threads[tid]->stack;
  delete threads[tid];
  threads[tid] = nullptr;
}

/**
 * * This function release all allocated memory, threads pointers and their
 * stacks
 */
static void free_all_threads()
{
  for(int i = 0; i < MAX_THREAD_NUM; i ++)
  {
    if(threads[i] != nullptr)
    {
      free_thread (i);
    }
  }
}

/**
 * A function that prints to the std::cerr a system error message in the next format:
 * "system error:" + msg
 * The function ends the program running
 * @param msg represents the message
 */
static void system_error_exit_func(const std::string& msg)
{
  std::cerr << SYSTEM_ERROR << msg << "\n";
  free_all_threads();
  exit(1);
}

/**
 * A function that prints to the std::cerr a system error message in the next format:
 * "system error:" + msg
 * @param msg represents the message
 * @return returns upon call FAILURE (-1)
 */
static int library_error_exit_func(const std::string& msg)
{
  std::cerr << THREAD_LIBRARY_ERROR << msg << "\n";
  return FAILURE;
}

/**
 * A function that finds an empty cell in the threads array with the lower index
 * @return the index of the cell upon success, FAILURE otherwise
 */
static int min_empty_index() {

  // searching for the min empty cell
  for (int i = 1; i < MAX_THREAD_NUM; i++)
    {
      if (threads[i] == nullptr) {return i;}
    }
  return FAILURE;
}

/**
 * A function that gets a tid and deletes the thread which its' id equals to tid
 * the function writen under assumption that the given tid exists
 * @param tid represents the threads id
 */
static void delete_from_queue(int tid)
{
  int counter = 0;
  for (auto &item : ready_threads)
    {
      if (item->id == tid) {
          ready_threads.erase (ready_threads.begin() + counter);
          return;
        }
      counter ++;
    }
}

/**
 * A function that checks if a thread with the given tid exists
 * @param tid represents the given thread ID
 * @return 0 upon success, -1 otherwise
 */
static int does_exists_tid(int tid)
{
  if (threads[tid] == nullptr)
    {
      std::cerr << THREAD_LIBRARY_ERROR << NO_TID << "\n";
      return FAILURE;
    }
  return SUCCESS;
}

/**
 * A function that pushes the thread with the given tid into the ready queue
 * the function assumes that a thread with given tid exists
 * @param tid tid represents the given thread ID
 */
static void push_thread_into_ready_queue(int tid)
{
  threads[tid]->stage = READY;
  ready_threads.push_back(threads[tid]);
}

/**
 * A function that replaces the running thread by a new thread
 */
static void change_context()
{
  // advancing the total_quantums counter
  total_quantums ++;

  // going through the sleeping threads and deletes and moves ready threads
  for (auto it = sleeping_threads.begin(); it != sleeping_threads.end();)
  {
    std::pair<int,int> p = it->second;
    if (p.first <= total_quantums && p.second == 0)
    {
      auto to_delete = it;
      push_thread_into_ready_queue (it->first);
      it++;
      sleeping_threads.erase (to_delete);
    }
    else {it++;}

  }

  // replacing the running thread
  running_thread = ready_threads.front();
  running_thread->quantum_counter ++;
  running_thread->stage = RUNNING;
  ready_threads.erase (ready_threads.begin());  // todo does it erases the pointer as well??

  // timer resetting:
  //  timer.it_interval.tv_sec = 0;
  //  timer.it_interval.tv_usec = quantum_usecs;
//  timer_init(quantums_secs);


//  timer.it_value.tv_sec = 0;        // first time interval, seconds part
//  timer.it_value.tv_usec = quantums_secs;        // first time interval, microseconds part

  sigprocmask(SIG_UNBLOCK, &mask, nullptr);

  // jumping
  siglongjmp(threads[running_thread->id]->data, 1);
}

/**
 * A function that goes in action when a thread is preempted
 * the function saving the current state of the running thread and replacing it by a new thread
 * the function assumes that the thread was preempted because of run time over or was blocked
 * @param sig
 */
static void sig_occurred(int sig = -1)
{
  // if do not need to execute the sig_occurred function
  if (!execute_timer)
  {
    execute_timer = true;
    return;
  }

  // saving current state
  int ret_val = sigsetjmp(threads[running_thread->id]->data, DEFAULT_VALUE);

  // jumping if needed
  bool did_just_save_bookmark = ret_val == ZERO;

  if (did_just_save_bookmark)
    {
      // pushing the old thread into the last place in the ready queue
       push_thread_into_ready_queue (running_thread->id);

      // replacing the running thread
      change_context();
    }
}

/**
 * A function that initiates the timer of the library
 * this function is being called from the uthreads_init func and sets a timer with an interval of
 * quantum_usecs micro-seconds.
 * @param quantum_usecs represents the number of quantum in micro-seconds.
 */
static void timer_init(int quantum_usecs)
{
  struct sigaction sa = {nullptr};
  // Install thread_is_preempted as the signal handler for SIGVTALRM.
  sa.sa_handler = &sig_occurred;
  if (sigaction(SIGVTALRM, &sa, nullptr) < 0)
    { system_error_exit_func(SIGACTION_ERROR); }

  // Configure the timer to expire after quantum_usecs secs
  timer.it_value.tv_sec = 0;        // first time interval, seconds part
  timer.it_value.tv_usec = quantum_usecs;        // first time interval, microseconds part

  // setting an interval for every quantum_usecs secs
  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = quantum_usecs;

  // Start a virtual timer. It counts down whenever this process is executing.
  if (setitimer(ITIMER_VIRTUAL, &timer, nullptr))
    { system_error_exit_func(SETITIMER_ERROR); }
}

/**
 * A helper function that unblocks the masked signals set and returns SUCCESS
 * @return SUCCESS (0)
 */
static int success_unblock_sig ()
{
  sigprocmask(SIG_UNBLOCK, &mask, nullptr);
  return SUCCESS;
}

/**
 * A helper function that unblocks the masked signals set and returns FAILURE
 * @return FAILURE (-1)
 */
static int failure_unblock_sig ()
{
  sigprocmask(SIG_UNBLOCK, &mask, nullptr);
  return FAILURE;
}

/**
 * A helper function that checks if a SIGVTALRM occurred during it was blocked and changes the execute_timer
 * var accordingly.
 */
static void sigvtalrm_occurred_deal()
{
  // getting the set of the pending signals
  if (sigemptyset(&pending_signals) != 0 )
  { system_error_exit_func (SIGEMPTYSET_ERROR); }

  if (sigpending(&pending_signals) != 0)
  { system_error_exit_func (SIGPENDING_ERROR); }

  // checking if a SIGVTALRM occurred
  if (sigismember(&pending_signals, SIGVTALRM))
  { execute_timer = false; }

}


int uthread_init(int quantum_usecs)
{
  // validity check
  if (quantum_usecs <= 0) { return library_error_exit_func (NON_POSITIVE_QUANTUM_USECS); }

  // initializing static vars
  ready_threads = std::vector<thread*>();
  sleeping_threads = std::map<int,std::pair<int, int>>();
  quantums_secs = quantum_usecs;
  total_quantums = 1;
  execute_timer = true;
  for (auto & thread : threads)
    { thread = nullptr;}

  // initializing the timer
  timer_init (quantum_usecs);

  // initiating the blocked signals set:
  if (sigemptyset(&mask) != 0 )
  { system_error_exit_func (SIGEMPTYSET_ERROR); }

  if (sigaddset(&mask, SIGVTALRM) != 0)
  { system_error_exit_func (SIGADDSET_ERROR); }

  // creating the Main thread
  threads[FIRST_ID] = new thread;
  *(threads[FIRST_ID]) = {1, FIRST_ID, RUNNING, 0, 0, nullptr};
  running_thread = threads[FIRST_ID];

  return SUCCESS;
}

int uthread_spawn(thread_entry_point entry_point)
{
  // Blocking unwanted signals to occur
  sigprocmask(SIG_BLOCK, &mask, nullptr);

  // validity check
  if (entry_point == nullptr)
  {
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    return library_error_exit_func(NULL_ENTRY_POINT);
  }

  // checking for empty space in the threads array
  int ind = min_empty_index();
  if (ind == FAILURE)
    {
      sigprocmask(SIG_UNBLOCK, &mask, nullptr);
      return library_error_exit_func(EXCEED_THE_LIMIT);
    }

  // creating the new thread and adding to the queue
  threads[ind] = new thread;
  threads[ind]->id = ind;
  threads[ind]->stage = READY;
  threads[ind]->quantum_counter = 0;
  threads[ind]->stack = new char [STACK_SIZE];
  threads[ind]->sp = (address_t) (threads[ind]->stack + STACK_SIZE - sizeof(address_t));
  threads[ind]->pc = (unsigned long)entry_point;
  sigsetjmp(threads[ind]->data, DEFAULT_VALUE);
  (threads[ind]->data->__jmpbuf)[JB_SP] = translate_address(threads[ind]->sp);
  (threads[ind]->data->__jmpbuf)[JB_PC] = translate_address(threads[ind]->pc);
  sigemptyset(&(threads[ind]->data->__saved_mask));
  ready_threads.push_back(threads[ind]);

  sigprocmask(SIG_UNBLOCK, &mask, nullptr);
  return ind;
}

int uthread_terminate(int tid)
{
  // Blocking unwanted signals to occur
  sigprocmask(SIG_BLOCK, &mask, nullptr);

  if (tid == 0)
  {
    free_all_threads();
    exit(0);
  }

  // checking if a thread with given tid exists
  if (does_exists_tid (tid) == FAILURE) { return failure_unblock_sig(); }

  bool running_flag = running_thread->id == tid;

  // deleting from the ready_queue and sleeping queue and deleting the thread
  if (threads[tid]->stage == READY) { delete_from_queue (tid); }
  if (threads[tid]->stage == BLOCKED) {sleeping_threads.erase (tid);}
  free_thread (tid);

  // changing the running thread if needed
  if (running_flag)
  {
    // if SIGVTALRM occurred need to block the context switch because it's already happened:
    sigvtalrm_occurred_deal();
    change_context();
  }

  sigprocmask(SIG_UNBLOCK, &mask, nullptr);
  return SUCCESS;
}

int uthread_block(int tid)
{
  // Blocking unwanted signals to occur
  sigprocmask(SIG_BLOCK, &mask, nullptr);

  // checking validity
  if (tid == 0) { return library_error_exit_func (BLOCKING_MAIN_THREAD); }

  // checking if a thread with given tid exists
  if (does_exists_tid (tid) == FAILURE) { return failure_unblock_sig(); }

  // checking if the stage of the given tid is ready
  if (threads[tid]->stage == READY) { delete_from_queue (tid); }

  // if already is blocked, setting the blocked restriction to blocked
  if (threads[tid]->stage == BLOCKED) { sleeping_threads[tid].second = BLOCKED; }

  // if not blocked, adding into the sleeping/blocked queue
  else {
    std::pair<int,int> restrictions = {0,BLOCKED};
    std::pair<int, std::pair<int,int>> item = {tid, restrictions};
    sleeping_threads.insert(item);
    threads[tid]->stage = BLOCKED;
  }

  // changing the running thread if needed, means if the running thread was blocked
  if (running_thread->id == tid)
    {
      int ret_val = sigsetjmp(threads[tid]->data, DEFAULT_VALUE);
      if (ret_val == 0)
      {
        // if SIGVTALRM occurred need to block the context switch because it's already happened:
        sigvtalrm_occurred_deal();
        change_context();
      }
    }

  sigprocmask(SIG_UNBLOCK, &mask, nullptr);
  return SUCCESS;
}

int uthread_resume(int tid)
{
  // Blocking unwanted signals to occur
  sigprocmask(SIG_BLOCK, &mask, nullptr);

  // checking if a thread with given tid exists
  if (does_exists_tid (tid) == FAILURE) { return failure_unblock_sig();  }

  int is_thread_sleeping = (int)sleeping_threads.count(tid);

  // if not blocked:
  if (is_thread_sleeping == 0) { return success_unblock_sig (); }

  // unblocking the thread from being blocked
  sleeping_threads[tid].second = UNBLOCKED;

  //checking if the thread can be moved from sleeping to ready
  if (sleeping_threads[tid].first == 0)
    {
      push_thread_into_ready_queue(tid);
      sleeping_threads.erase (tid);
    }

  // unblocking the signals
  sigprocmask(SIG_UNBLOCK, &mask, nullptr);

  return SUCCESS;
}

int uthread_sleep(int num_quantums)
{
  // Blocking unwanted signals to occur
  sigprocmask(SIG_BLOCK, &mask, nullptr);

  // validity check
  if (running_thread->id == 0)
  {
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
    return library_error_exit_func (SLEEP_MAIN_THREAD);
  }

  int tid = running_thread->id;

  // blocking the thread and inserting into the sleeping threads Data Structure:
  std::pair<int,int> restrictions = {num_quantums + total_quantums, 0};
  std::pair<int, std::pair<int,int>> item = {tid, restrictions};
  sleeping_threads.insert(item);
  threads[tid]->stage = BLOCKED;

  int ret_val = sigsetjmp(threads[tid]->data, DEFAULT_VALUE);

  // changing the running thread
  if (ret_val == 0)
  {
    // if SIGVTALRM occurred need to block the context switch because it's already happened:
    sigvtalrm_occurred_deal();
    change_context();
  }

  sigprocmask(SIG_UNBLOCK, &mask, nullptr);
  return SUCCESS; // todo
}

int uthread_get_tid() { return running_thread->id; }

int uthread_get_total_quantums() { return total_quantums; }

int uthread_get_quantums(int tid)
{
  if (does_exists_tid (tid) != FAILURE ) { return threads[tid]->quantum_counter; }
  return FAILURE;
}



