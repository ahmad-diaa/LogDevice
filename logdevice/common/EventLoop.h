/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <pthread.h>
#include <semaphore.h>
#include <unordered_map>

#include <folly/Executor.h>

#include "logdevice/common/BatchedBufferDisposer.h"
#include "logdevice/common/Semaphore.h"
#include "logdevice/common/ThreadID.h"
#include "logdevice/common/TimeoutMap.h"
#include "logdevice/common/ZeroCopyPayload.h"

struct event_base;
struct event;

namespace facebook { namespace logdevice {

/**
 * @file   an EventLoop is a LogDevice internal thread running a libevent 2.x
 *         event_base. All LogDevice requests are executed on EventLoop
 *         threads. EventLoop objects directly receive and execute requests
 *         from client threads, the listener thread, and the command port
 *         thread.
 */

class EventLoopTaskQueue;

class EventLoop : public folly::Executor {
 public:
  /**
   * Creates and starts the EventLoop's thread.  The thread does not
   * immediately start running the event loop, that only happens after start()
   * is called.
   *
   * @throws ConstructorFailed on error, sets err to
   *   NOMEM    if a libevent call failed because malloc() failed
   *   INTERNAL if a libevent call fails unexpectedly
   *   SYSLIMIT if system limits on thread stack sizes or total number of
   *            threads (if any) are reached
   */
  explicit EventLoop(
      std::string thread_name = "",
      ThreadID::Type thread_type = ThreadID::Type::UNKNOWN_EVENT_LOOP,
      size_t request_pump_capacity = 1024,
      int requests_per_iteration = 16);

  // destructor has to be virtual because it is invoked by EventLoop::run()
  // as "delete this"
  virtual ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop& operator=(EventLoop&&) = delete;

  struct event_base* getEventBase() {
    return base_.get();
  }

  /// Enqueue a function to executed by this executor. This and all
  /// variants must be threadsafe.
  void add(folly::Function<void()>) override;

  /**
   * Enqueue function in scheduler with priority. Executor will enqueue it in
   * priortized fashion. Default implementation does not honor priority and just
   * calls EventLoop::add.
   */
  void addWithPriority(folly::Function<void()>, int8_t priority) override;

  /**
   * Get the thread handle of this EventLoop.
   *
   * @return the pthread handle. This function should never fail.
   */
  pthread_t getThread() const {
    return thread_;
  }

  int getThreadId() const {
    return tid_;
  }

  EventLoopTaskQueue& getTaskQueue() {
    return *task_queue_;
  }

  /**
   * @return   a pointer to the EventLoop object running on this thread, or
   *           nullptr if this thread is not running a EventLoop.
   */
  static EventLoop* onThisThread() {
    return EventLoop::thisThreadLoop;
  }

  void dispose(ZeroCopyPayload* payload);

  const BatchedBufferDisposer<ZeroCopyPayload>& disposer() const {
    return disposer_;
  }

  // A map that translates std::chrono::milliseconds values into
  // struct timevals suitable for use with evtimer_add() for append request
  // timers. The first kMaxFastTimeouts *distinct* timeout values are
  // mapped into fake struct timeval created by
  // event_base_init_common_timeout() and actually containing timer queue
  // ids for this thread's event_base.
  TimeoutMap& commonTimeouts() {
    return common_timeouts_;
  }

  // Convenience function so callers of commonTimeouts().get() don't need
  // to declare a local timeval. Must only be used from the Worker's thread.
  template <typename Duration>
  const timeval* getCommonTimeout(Duration d) {
    ld_check(EventLoop::onThisThread() == this);
    auto timeout = std::chrono::duration_cast<std::chrono::microseconds>(d);
    return commonTimeouts().get(timeout);
  }
  const timeval* getZeroTimeout() {
    return commonTimeouts().get(std::chrono::milliseconds(0));
  }

  static const int PRIORITY_LOW = 2;    // lowest priority
  static const int PRIORITY_NORMAL = 1; // default libevent priority
  static const int PRIORITY_HIGH = 0;   // elevated priority (numerically lower)

  static const int NUM_PRIORITIES = PRIORITY_LOW + 1;

 private:
  // libevent 2.x event_base that runs the loop. We use a unique_ptr with a
  // deleter and make base_ first data member of this class to make sure it
  // is deleted after all of the events in it.
  std::unique_ptr<event_base, std::function<void(event_base*)>> base_;

 public:
  // total number of event handlers that libevent has called so far
  std::atomic<size_t> event_handlers_called_{0};
  // total number of event handlerss that finished processing and returned
  // control to libevent
  std::atomic<size_t> event_handlers_completed_{0};

  // Delay in running a default priority event by EventLoo0p
  std::atomic<std::chrono::microseconds> delay_us_{
      std::chrono::microseconds(0)};

 protected:
  // called on this EventLoop's thread before starting the event loop
  virtual void onThreadStarted() {}

  bool keepAliveAcquire() override {
    if (shutting_down_.load()) {
      return false;
    }

    num_references_++;
    return true;
  }

  void keepAliveRelease() override {
    auto prev = num_references_.fetch_sub(1);
    ld_assert(prev > 0);
  }

 private:
  ThreadID::Type thread_type_;
  std::string thread_name_;

  pthread_t thread_; // thread on which this loop runs

  // pid of thread_
  int tid_{-1};

  // Main task queue; (shutting down this TaskQueue stops the event loop)
  std::unique_ptr<EventLoopTaskQueue> task_queue_;

  std::atomic<bool> running_;
  std::atomic<bool> shutting_down_;

  // entry point of the loop's thread
  static void* enter(void* self);

  // called by enter() to run this event loop on .thread_
  void run();

  // this is how a thread finds if it's running an EventLoop, and which one
  static __thread EventLoop* thisThreadLoop;

  // stack size of this loop's thread (pthread defaults are low)
  static const size_t STACK_SIZE = (1024 * 1024);

  // Semaphore that coordinates initialization and starting of the event loop.
  Semaphore start_sem_;

  // Constantly repeating event to calculate delay in event loop runs.
  // Every 1s schedules a zero timeout event and notes delays in
  // executing this event. This indicates how long it takes to service a active
  // event on eventloop
  static void delayCheckCallback(void* arg, short);
  struct event* scheduled_event_;
  std::chrono::steady_clock::time_point scheduled_event_start_time_{
      std::chrono::steady_clock::time_point::min()};

  // Counter to keep track of number of work contexts that depend on the
  // eventloop.
  std::atomic<size_t> num_references_{0};

  // Batched disposer to delete records on this event base in a batch.
  BatchedBufferDisposer<ZeroCopyPayload> disposer_;

  // TimeoutMap to cache common timeouts.
  TimeoutMap common_timeouts_;

  // Size limit for commonTimeouts_ (NB: libevent has a default upper bound
  // of MAX_COMMON_TIMEOUTS = 256)
  static const int kMaxFastTimeouts = 200;
};

}} // namespace facebook::logdevice
