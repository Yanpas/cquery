#pragma once

#include "utils.h"

#include <optional.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <tuple>
#include <utility>

// TODO: cleanup includes.

struct BaseThreadQueue {
  virtual bool IsEmpty() = 0;
  virtual ~BaseThreadQueue() = default;
};

struct MultiQueueWaiter {
  std::condition_variable_any cv;

  void Wait(std::initializer_list<BaseThreadQueue*> queues);
};

// A threadsafe-queue. http://stackoverflow.com/a/16075550
template <class T>
struct ThreadedQueue : public BaseThreadQueue {
 public:
  ThreadedQueue() : total_count_(0) {
    owned_waiter_ = std::make_unique<MultiQueueWaiter>();
    waiter_ = owned_waiter_.get();
  }

  explicit ThreadedQueue(MultiQueueWaiter* waiter)
      : total_count_(0), waiter_(waiter) {}

  // Returns the number of elements in the queue. This is lock-free.
  size_t Size() const { return total_count_; }

  // Add an element to the queue.
  void Enqueue(T&& t, bool priority) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (priority)
        priority_.push_back(std::move(t));
      else
        queue_.push_back(std::move(t));
      ++total_count_;
    }
    waiter_->cv.notify_all();
  }

  // Add a set of elements to the queue.
  void EnqueueAll(std::vector<T>&& elements, bool priority) {
    if (elements.empty())
      return;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      total_count_ += elements.size();
      for (T& element : elements) {
        if (priority)
          priority_.push_back(std::move(element));
        else
          queue_.push_back(std::move(element));
      }
      elements.clear();
    }

    waiter_->cv.notify_all();
  }

  // Return all elements in the queue.
  std::vector<T> DequeueAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    total_count_ = 0;

    std::vector<T> result;
    result.reserve(priority_.size() + queue_.size());
    while (!priority_.empty()) {
      result.emplace_back(std::move(priority_.front()));
      priority_.pop_front();
    }
    while (!queue_.empty()) {
      result.emplace_back(std::move(queue_.front()));
      queue_.pop_front();
    }

    return result;
  }

  // Returns true if the queue is empty. This is lock-free.
  bool IsEmpty() { return total_count_ == 0; }

  // Get the first element from the queue. Blocks until one is available.
  T Dequeue() {
    std::unique_lock<std::mutex> lock(mutex_);
    waiter_->cv.wait(lock,
                     [&]() { return !priority_.empty() || !queue_.empty(); });

    auto execute = [&](std::deque<T>* q) {
      auto val = std::move(q->front());
      q->pop_front();
      --total_count_;
      return std::move(val);
    };
    if (!priority_.empty())
      return execute(&priority_);
    return execute(&queue_);
  }

  // Get the first element from the queue without blocking. Returns a null
  // value if the queue is empty.
  optional<T> TryPop(bool priority) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto pop = [&](std::deque<T>* q) {
      auto val = std::move(q->front());
      q->pop_front();
      --total_count_;
      return std::move(val);
    };

    auto get_result = [&](std::deque<T>* first, std::deque<T>* second) -> optional<T> {
      if (!first->empty())
        return pop(first);
      if (!second->empty())
        return pop(second);
      return nullopt;
    };

    if (priority)
      return get_result(&priority_, &queue_);
    return get_result(&queue_, &priority_);
  }

  template <typename Fn>
  void Iterate(Fn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : priority_)
      fn(entry);
    for (auto& entry : queue_)
      fn(entry);
  }

  mutable std::mutex mutex_;

 private:
  std::atomic<int> total_count_;
  std::deque<T> priority_;
  std::deque<T> queue_;
  MultiQueueWaiter* waiter_;
  std::unique_ptr<MultiQueueWaiter> owned_waiter_;
};
