/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_CYCLES_TBB_COMPAT

#  include "util/tbb_compat.h"

#  include <condition_variable>

#  ifdef _WIN32
#    include "util/windows.h"
#  endif

#  if defined(__SSE__) || defined(_M_IX86) || defined(_M_X64)
#    include <xmmintrin.h>
#    define TBB_COMPAT_MXCSR
#  endif

namespace tbb::compat {

static std::atomic<int> max_allowed = 0;

static thread_local int thread_index = 0;
static thread_local bool in_parallel_run = false;
static thread_local task_group_context *context = nullptr;
static thread_local int arena_limit = 0;
static thread_local task_group *group = nullptr;

int &arena_concurrency()
{
  return arena_limit;
}

void set_max_allowed_parallelism(const int num_threads)
{
  max_allowed = num_threads;
}

int max_allowed_parallelism()
{
  return max_allowed;
}

/* One thread per core: Hyper-Threading siblings share the core's floating point units, two Cycles
 * threads on one Pentium 4 core render no faster than one. The other one is left to Blender. */
static int hardware_threads()
{
  int num_threads = int(std::thread::hardware_concurrency());
#  ifdef _WIN32
  DWORD size = 0;
  GetLogicalProcessorInformation(nullptr, &size);
  std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
      size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
  if (!info.empty() && GetLogicalProcessorInformation(info.data(), &size)) {
    int cores = 0;
    for (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION &processor : info) {
      cores += (processor.Relationship == RelationProcessorCore);
    }
    if (cores > 0) {
      num_threads = cores;
    }
  }
#  endif
  return std::max(num_threads, 1);
}

int max_threads()
{
  static const int default_threads = hardware_threads();
  int num_threads = (max_allowed > 0) ? int(max_allowed) : default_threads;
  if (arena_limit > 0) {
    num_threads = std::min(num_threads, arena_limit);
  }
  return std::max(num_threads, 1);
}

int current_thread_index()
{
  return thread_index;
}

task_group_context *current_context()
{
  return context;
}

task_group *&current_task_group()
{
  return group;
}

int64_t num_chunks(const int64_t size, const int64_t grainsize)
{
  if (size <= 0) {
    return 0;
  }
  const int threads = max_threads();
  if (threads <= 1) {
    return 1;
  }
  /* Enough chunks to balance the threads when some chunks take longer. */
  const int64_t pieces = std::max(int64_t(1), size / std::max(grainsize, int64_t(1)));
  return std::min(pieces, int64_t(threads) * 16);
}

/* Threads that help the thread calling `parallel_run`, one job at a time. Never freed: the
 * threads wait on it until the process exits. */
class Pool {
 public:
  std::mutex job_mutex;

  void run(const int num_threads,
           const int64_t num_chunks,
           const std::function<void(int64_t)> &fn,
           task_group_context &job_context)
  {
    while (int(workers_) < num_threads - 1) {
      workers_++;
      std::thread([this, index = int(workers_)] { worker(index); }).detach();
    }
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      fn_ = &fn;
      context_ = &job_context;
      next_ = 0;
      num_chunks_ = num_chunks;
      helpers_ = int(std::min(int64_t(num_threads - 1), num_chunks - 1));
      helpers_done_ = 0;
#  ifdef TBB_COMPAT_MXCSR
      /* Like TBB, the helpers use the floating point settings of the calling thread. */
      mxcsr_ = _mm_getcsr();
#  endif
      generation_++;
    }
    work_cond_.notify_all();

    work();

    std::unique_lock<std::mutex> lock(mutex_);
    done_cond_.wait(lock, [this] { return helpers_done_ == helpers_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable work_cond_;
  std::condition_variable done_cond_;
  int workers_ = 0;
  uint64_t generation_ = 0;

  const std::function<void(int64_t)> *fn_ = nullptr;
  task_group_context *context_ = nullptr;
  std::atomic<int64_t> next_ = 0;
  int64_t num_chunks_ = 0;
  int helpers_ = 0;
  int helpers_done_ = 0;
#  ifdef TBB_COMPAT_MXCSR
  unsigned int mxcsr_ = 0;
#  endif

  void work()
  {
    while (!context_->is_group_execution_cancelled()) {
      const int64_t chunk = next_++;
      if (chunk >= num_chunks_) {
        break;
      }
      (*fn_)(chunk);
    }
  }

  void worker(const int index)
  {
    thread_index = index;
    uint64_t seen_generation = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      work_cond_.wait(lock, [&] { return generation_ != seen_generation; });
      seen_generation = generation_;
      if (index > helpers_) {
        continue;
      }
      lock.unlock();
#  ifdef TBB_COMPAT_MXCSR
      _mm_setcsr(mxcsr_);
#  endif
      in_parallel_run = true;
      context = context_;
      work();
      context = nullptr;
      in_parallel_run = false;
      lock.lock();
      if (++helpers_done_ == helpers_) {
        done_cond_.notify_one();
      }
    }
  }
};

static Pool &pool()
{
  static Pool *pool = new Pool();
  return *pool;
}

static void run_serial(const int64_t num_chunks, const std::function<void(int64_t)> &fn)
{
  task_group_context job_context;
  task_group_context *previous = context;
  /* Inside a parallel loop, canceling cancels that loop. */
  task_group_context *current = (previous != nullptr) ? previous : &job_context;
  context = current;
  for (int64_t chunk = 0; chunk < num_chunks && !current->is_group_execution_cancelled();
       chunk++)
  {
    fn(chunk);
  }
  context = previous;
}

void parallel_run(const int64_t num_chunks, const std::function<void(int64_t)> &fn)
{
  if (num_chunks <= 0) {
    return;
  }
  const int num_threads = max_threads();
  if (num_chunks == 1 || num_threads <= 1 || in_parallel_run) {
    run_serial(num_chunks, fn);
    return;
  }
  Pool &threads = pool();
  /* Another thread runs a loop on the pool. */
  std::unique_lock<std::mutex> job_lock(threads.job_mutex, std::try_to_lock);
  if (!job_lock.owns_lock()) {
    run_serial(num_chunks, fn);
    return;
  }

  task_group_context job_context;
  task_group_context *previous_context = context;
  const int previous_index = thread_index;
  context = &job_context;
  thread_index = 0;
  in_parallel_run = true;
  threads.run(num_threads, num_chunks, fn, job_context);
  in_parallel_run = false;
  thread_index = previous_index;
  context = previous_context;
}

}  // namespace tbb::compat

#endif /* WITH_CYCLES_TBB_COMPAT */
