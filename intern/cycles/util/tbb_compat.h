/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* The part of TBB that Cycles uses, for builds without TBB (the Windows XP port, whose TBB
 * runtime would need Vista).
 *
 * Parallel loops run on a small pool of threads: one fewer than the allowed number of threads,
 * the calling thread is the other one. A loop started while another one runs (inside its body,
 * or from another thread) runs serially on its thread. Task groups run their tasks right away, on
 * the thread that adds them. */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#define TBB_INTERFACE_VERSION_MAJOR 12

namespace tbb {

class task_group_context;

namespace compat {

/* Run `fn(chunk)` for chunks `0..num_chunks-1`, on up to `max_threads` threads. */
void parallel_run(int64_t num_chunks, const std::function<void(int64_t chunk)> &fn);

/* Number of chunks to split `size` items in, each at least `grainsize` items. */
int64_t num_chunks(int64_t size, int64_t grainsize);

/* Threads a parallel loop may use: the arena's, within the global limit. */
int max_threads();
int current_thread_index();
task_group_context *current_context();

/* The limit of the arena whose `execute` runs on this thread, 0 outside of arenas. */
int &arena_concurrency();
/* Parallelism allowed by `global_control`, 0 for the number of hardware threads. */
void set_max_allowed_parallelism(int num_threads);
int max_allowed_parallelism();

template<typename Value>
std::pair<Value, Value> chunk_range(const Value begin,
                                    const Value end,
                                    const int64_t chunk,
                                    const int64_t chunks)
{
  const int64_t size = int64_t(end - begin);
  return {Value(begin + size * chunk / chunks), Value(begin + size * (chunk + 1) / chunks)};
}

}  // namespace compat

class task_group_context {
 public:
  void capture_fp_settings() {}
  void cancel_group_execution()
  {
    canceled_ = true;
  }
  bool is_group_execution_cancelled() const
  {
    return canceled_;
  }

 private:
  std::atomic<bool> canceled_ = false;
};

namespace task {
inline task_group_context *current_context()
{
  return compat::current_context();
}
}  // namespace task

/* -------------------------------------------------------------------- */
/** \name Ranges
 * \{ */

template<typename Value> class blocked_range {
 public:
  blocked_range(const Value begin, const Value end, const size_t grainsize = 1)
      : begin_(begin), end_(end), grainsize_(grainsize)
  {
  }

  Value begin() const
  {
    return begin_;
  }
  Value end() const
  {
    return end_;
  }
  size_t size() const
  {
    return size_t(end_ - begin_);
  }
  bool empty() const
  {
    return !(begin_ < end_);
  }
  size_t grainsize() const
  {
    return grainsize_;
  }

  /* Split in chunks of whole `grainsize` pieces. */
  int64_t compat_num_chunks() const
  {
    return empty() ? 0 : compat::num_chunks(int64_t(size()), int64_t(grainsize_));
  }
  blocked_range compat_chunk(const int64_t chunk, const int64_t chunks) const
  {
    const auto [begin, end] = compat::chunk_range(begin_, end_, chunk, chunks);
    return blocked_range(begin, end, grainsize_);
  }

 private:
  Value begin_;
  Value end_;
  size_t grainsize_;
};

/* Split along rows. */
template<typename RowValue, typename ColValue = RowValue> class blocked_range2d {
 public:
  blocked_range2d(const RowValue row_begin,
                  const RowValue row_end,
                  const size_t row_grainsize,
                  const ColValue col_begin,
                  const ColValue col_end,
                  const size_t col_grainsize)
      : rows_(row_begin, row_end, row_grainsize), cols_(col_begin, col_end, col_grainsize)
  {
  }
  blocked_range2d(const RowValue row_begin,
                  const RowValue row_end,
                  const ColValue col_begin,
                  const ColValue col_end)
      : rows_(row_begin, row_end), cols_(col_begin, col_end)
  {
  }
  blocked_range2d(const blocked_range<RowValue> &rows, const blocked_range<ColValue> &cols)
      : rows_(rows), cols_(cols)
  {
  }

  const blocked_range<RowValue> &rows() const
  {
    return rows_;
  }
  const blocked_range<ColValue> &cols() const
  {
    return cols_;
  }
  bool empty() const
  {
    return rows_.empty() || cols_.empty();
  }

  int64_t compat_num_chunks() const
  {
    return cols_.empty() ? 0 : rows_.compat_num_chunks();
  }
  blocked_range2d compat_chunk(const int64_t chunk, const int64_t chunks) const
  {
    return blocked_range2d(rows_.compat_chunk(chunk, chunks), cols_);
  }

 private:
  blocked_range<RowValue> rows_;
  blocked_range<ColValue> cols_;
};

/* Split along pages. */
template<typename PageValue, typename RowValue = PageValue, typename ColValue = RowValue>
class blocked_range3d {
 public:
  blocked_range3d(const PageValue page_begin,
                  const PageValue page_end,
                  const size_t page_grainsize,
                  const RowValue row_begin,
                  const RowValue row_end,
                  const size_t row_grainsize,
                  const ColValue col_begin,
                  const ColValue col_end,
                  const size_t col_grainsize)
      : pages_(page_begin, page_end, page_grainsize),
        rows_(row_begin, row_end, row_grainsize),
        cols_(col_begin, col_end, col_grainsize)
  {
  }
  blocked_range3d(const PageValue page_begin,
                  const PageValue page_end,
                  const RowValue row_begin,
                  const RowValue row_end,
                  const ColValue col_begin,
                  const ColValue col_end)
      : pages_(page_begin, page_end), rows_(row_begin, row_end), cols_(col_begin, col_end)
  {
  }
  blocked_range3d(const blocked_range<PageValue> &pages,
                  const blocked_range<RowValue> &rows,
                  const blocked_range<ColValue> &cols)
      : pages_(pages), rows_(rows), cols_(cols)
  {
  }

  const blocked_range<PageValue> &pages() const
  {
    return pages_;
  }
  const blocked_range<RowValue> &rows() const
  {
    return rows_;
  }
  const blocked_range<ColValue> &cols() const
  {
    return cols_;
  }
  bool empty() const
  {
    return pages_.empty() || rows_.empty() || cols_.empty();
  }

  int64_t compat_num_chunks() const
  {
    return (rows_.empty() || cols_.empty()) ? 0 : pages_.compat_num_chunks();
  }
  blocked_range3d compat_chunk(const int64_t chunk, const int64_t chunks) const
  {
    return blocked_range3d(pages_.compat_chunk(chunk, chunks), rows_, cols_);
  }

 private:
  blocked_range<PageValue> pages_;
  blocked_range<RowValue> rows_;
  blocked_range<ColValue> cols_;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Algorithms
 * \{ */

template<typename Range, typename Body> void parallel_for(const Range &range, const Body &body)
{
  const int64_t chunks = range.compat_num_chunks();
  compat::parallel_run(chunks, [&](const int64_t chunk) {
    body(range.compat_chunk(chunk, chunks));
  });
}

template<typename Index, typename Function>
void parallel_for(const Index first, const Index last, const Function &f)
{
  if (!(first < last)) {
    return;
  }
  const int64_t chunks = compat::num_chunks(int64_t(last - first), 1);
  compat::parallel_run(chunks, [&](const int64_t chunk) {
    const auto [begin, end] = compat::chunk_range(first, last, chunk, chunks);
    for (Index i = begin; i < end; i++) {
      f(i);
    }
  });
}

template<typename Iterator, typename Function>
void parallel_for_each(const Iterator first, const Iterator last, const Function &f)
{
  std::vector<Iterator> items;
  for (Iterator it = first; it != last; ++it) {
    items.push_back(it);
  }
  parallel_for(size_t(0), items.size(), [&](const size_t i) { f(*items[i]); });
}

template<typename Container, typename Function>
void parallel_for_each(Container &container, const Function &f)
{
  parallel_for_each(std::begin(container), std::end(container), f);
}

template<typename Range, typename Value, typename RealBody, typename Reduction>
Value parallel_reduce(const Range &range,
                      const Value &identity,
                      const RealBody &real_body,
                      const Reduction &reduction)
{
  const int64_t chunks = range.compat_num_chunks();
  std::vector<Value> results(size_t(chunks), identity);
  compat::parallel_run(chunks, [&](const int64_t chunk) {
    results[chunk] = real_body(range.compat_chunk(chunk, chunks), identity);
  });
  Value result = identity;
  for (const Value &value : results) {
    result = reduction(result, value);
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Arenas and Thread Control
 * \{ */

class task_arena {
 public:
  static constexpr int automatic = -1;

  explicit task_arena(const int max_concurrency = automatic)
      : max_concurrency_(max_concurrency > 0 ? max_concurrency : 0)
  {
  }

  template<typename F> auto execute(const F &f) -> decltype(f())
  {
    int &concurrency = compat::arena_concurrency();
    const int previous = concurrency;
    concurrency = max_concurrency_;
    struct Restore {
      int &concurrency;
      int previous;
      ~Restore()
      {
        concurrency = previous;
      }
    } restore{concurrency, previous};
    return f();
  }

 private:
  int max_concurrency_;
};

namespace this_task_arena {
inline int max_concurrency()
{
  return compat::max_threads();
}
inline int current_thread_index()
{
  return compat::current_thread_index();
}
template<typename F> auto isolate(const F &f) -> decltype(f())
{
  return f();
}
}  // namespace this_task_arena

class global_control {
 public:
  enum parameter { max_allowed_parallelism };

  global_control(const parameter /*param*/, const size_t value)
      : previous_(compat::max_allowed_parallelism())
  {
    compat::set_max_allowed_parallelism(int(value));
  }
  ~global_control()
  {
    compat::set_max_allowed_parallelism(previous_);
  }

 private:
  int previous_;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Task Groups
 * \{ */

class task_group;

namespace compat {
task_group *&current_task_group();
}

/* Runs the tasks right away, on the thread that adds them. */
class task_group {
 public:
  template<typename F> void run(F &&f)
  {
    if (canceled_) {
      return;
    }
    task_group *&current = compat::current_task_group();
    task_group *previous = current;
    current = this;
    f();
    current = previous;
  }

  void wait()
  {
    canceled_ = false;
  }

  void cancel()
  {
    canceled_ = true;
  }

  bool is_canceling() const
  {
    return canceled_;
  }

 private:
  std::atomic<bool> canceled_ = false;
};

inline bool is_current_task_group_canceling()
{
  const task_group *group = compat::current_task_group();
  return group != nullptr && group->is_canceling();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Containers and Locks
 * \{ */

/* One value per thread that asks for it. */
template<typename T> class enumerable_thread_specific {
 public:
  T &local()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::unique_ptr<T> &value = values_[std::this_thread::get_id()];
    if (!value) {
      value = std::make_unique<T>();
    }
    return *value;
  }

  void clear()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
  }

 private:
  std::mutex mutex_;
  std::map<std::thread::id, std::unique_ptr<T>> values_;
};

class spin_mutex {
 public:
  void lock()
  {
    while (locked_.exchange(true, std::memory_order_acquire)) {
      while (locked_.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
      }
    }
  }
  bool try_lock()
  {
    return !locked_.exchange(true, std::memory_order_acquire);
  }
  void unlock()
  {
    locked_.store(false, std::memory_order_release);
  }

  class scoped_lock {
   public:
    explicit scoped_lock(spin_mutex &mutex) : mutex_(mutex)
    {
      mutex_.lock();
    }
    ~scoped_lock()
    {
      mutex_.unlock();
    }

   private:
    spin_mutex &mutex_;
  };

 private:
  std::atomic<bool> locked_ = false;
};

/** \} */

}  // namespace tbb
