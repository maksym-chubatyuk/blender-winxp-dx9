/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup creator
 */

#ifndef WITH_PYTHON_MODULE

#  include <cerrno>
#  include <cstdlib>
#  include <mutex>

#  if defined(__linux__) && defined(__GNUC__)
#    ifndef _GNU_SOURCE
#      define _GNU_SOURCE
#    endif
#    include <cfenv>
#  endif

#  if (defined(__APPLE__) && (defined(__i386__) || defined(__x86_64__)))
#    define OSX_SSE_FPE
#    include <xmmintrin.h>
#  endif

#  ifdef WIN32
#    include <float.h>
#    include <windows.h>
#    ifdef __MINGW32__
#      include <mmsystem.h>
#      include <tlhelp32.h>
#    endif

#    include "BLI_winstuff.h"

#    include "GPU_platform.hh"
#  endif

#  include "BLI_fileops.h"
#  include "BLI_path_utils.hh"
#  include "BLI_string.h"
#  include "BLI_system.h"
#  include BLI_SYSTEM_PID_H

#  include "BKE_appdir.hh" /* #BKE_tempdir_session_purge. */
#  include "BKE_blender.hh"
#  include "BKE_blender_version.h"
#  include "BKE_global.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"
#  include "BKE_wm_runtime.hh"

#  include <csignal>

#  ifdef WITH_PYTHON
#    include "BPY_extern_python.hh" /* #BPY_python_backtrace. */
#  endif

#  include "creator_intern.h" /* Own include. */

namespace blender {

#  if defined(__linux__) || defined(_WIN32) || defined(OSX_SSE_FPE)
/**
 * Set breakpoints here when running in debug mode, useful to catch floating point errors.
 */
static void sig_handle_fpe(int /*sig*/)
{
  fprintf(stderr, "debug: SIGFPE trapped\n");
}
#  endif

/* Handling `Ctrl-C` event in the console. */
static void sig_handle_blender_esc(int sig)
{
  /* Forces render loop to read queue, not sure if its needed. */
  G.is_break = true;

  if (sig == 2) {
    static int count = 0;
    if (count) {
      printf("\nBlender killed\n");
      exit(2);
    }
    printf("\nSent an internal break event. Press ^C again to kill Blender\n");
    count++;
  }
}

static void crashlog_file_generate(const char *filepath, const void *os_info)
{
  /* Might be called after WM/Main exit, so needs to be careful about nullptr-checking before
   * de-referencing. */

  wmWindowManager *wm = G_MAIN ? static_cast<wmWindowManager *>(G_MAIN->wm.first) : nullptr;

  FILE *fp;
  char header[512];
  if (!app_state.signal.use_console_crash_handler) {
    printf("Writing: %s\n", filepath);
  }
  fflush(stdout);

#  ifndef BUILD_DATE
  SNPRINTF(header, "# " BLEND_VERSION_FMT ", Unknown revision\n", BLEND_VERSION_ARG);
#  else
  SNPRINTF(header,
           "# " BLEND_VERSION_FMT ", Commit date: %s %s, Hash %s\n",
           BLEND_VERSION_ARG,
           build_commit_date,
           build_commit_time,
           build_hash);
#  endif

  /* Open the crash log. */
  errno = 0;
  if (app_state.signal.use_console_crash_handler) {
    fp = stderr;
  }
  else {
    fp = BLI_fopen(filepath, "wb");
    if (fp == nullptr) {
      fprintf(stderr,
              "Unable to save '%s': %s , falling back to console\n",
              filepath,
              errno ? strerror(errno) : "Unknown error opening file");
      fp = stderr;
    }
  }

  if (wm) {
    BKE_report_write_file_fp(fp, &wm->runtime->reports, header);
  }

  fputs("\n# backtrace\n", fp);
  BLI_system_backtrace_with_os_info(fp, os_info);

#  ifdef WITH_PYTHON
  /* Generate python back-trace if Python is currently active. */
  BPY_python_backtrace(fp);
#  endif
  if (fp != stderr) {
    fclose(fp);
  }
}

static void sig_cleanup_and_terminate(int signum)
{
  /* Delete content of temp directory. */
  BKE_tempdir_session_purge();

  /* Really crash. */
  signal(signum, SIG_DFL);
#  ifndef WIN32
  kill(getpid(), signum);
#  else
  TerminateProcess(GetCurrentProcess(), signum);
#  endif
}
#  if !defined(WIN32)
static void sig_handle_crash_fn(int signum)
{
  auto crash_func = [&]() {
    char filepath_crashlog[FILE_MAX];
    BKE_blender_globals_crash_path_get(filepath_crashlog);
    crashlog_file_generate(filepath_crashlog, nullptr);
    sig_cleanup_and_terminate(signum);
  };

  /* The use of `std::call_once` ensures that the crash handling function is only executed once,
   * even if multiple crashes occur simultaneously. */
  static std::once_flag crash_func_once;
  std::call_once(crash_func_once, crash_func);
}
#  else
#    ifdef __MINGW32__
/* XP port: print the values on a stack that point into the executable (a superset of the call
 * chain), without allocating. */
static void xp_print_stack_values(const uintptr_t *sp)
{
  char line[16];
  DWORD written;
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(nullptr);
  const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)dos + dos->e_lfanew);
  const uintptr_t image_start = uintptr_t(dos);
  const uintptr_t image_end = image_start + nt->OptionalHeader.SizeOfImage;
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(sp, &mbi, sizeof(mbi))) {
    const uintptr_t *end = (const uintptr_t *)((const char *)mbi.BaseAddress + mbi.RegionSize);
    for (int n = 0; sp < end && n < 300; sp++) {
      if (*sp >= image_start && *sp < image_end) {
        const int len = snprintf(line, sizeof(line), " %08lx", (unsigned long)*sp);
        WriteFile(err, line, DWORD(len), &written, nullptr);
        n++;
      }
    }
  }
  WriteFile(err, "\n", 1, &written, nullptr);
}

/* XP port: there is no working debugger on the XP machine. With `XP_THREAD_DUMP_AFTER=<seconds>`
 * the stacks of all threads are printed after that many seconds, to find where Blender hangs. */
static DWORD WINAPI xp_thread_dump(LPVOID seconds)
{
  Sleep(DWORD(uintptr_t(seconds)) * 1000);
  const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry;
  entry.dwSize = sizeof(entry);
  for (BOOL found = Thread32First(snapshot, &entry); found; found = Thread32Next(snapshot, &entry))
  {
    if (entry.th32OwnerProcessID != GetCurrentProcessId() ||
        entry.th32ThreadID == GetCurrentThreadId())
    {
      continue;
    }
    const HANDLE thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
    if (thread == nullptr) {
      continue;
    }
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(thread, &context)) {
        char line[64];
        DWORD written;
        const int len = snprintf(line,
                                 sizeof(line),
                                 "Thread %lu at 0x%08lx, stack:",
                                 (unsigned long)entry.th32ThreadID,
                                 (unsigned long)context.Eip);
        WriteFile(err, line, DWORD(len), &written, nullptr);
        xp_print_stack_values((const uintptr_t *)context.Esp);
      }
      ResumeThread(thread);
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
  return 0;
}

/* XP port: a sampling profiler. With `XP_PROFILE=<delay>,<seconds>` the instruction pointers of
 * all threads are sampled every millisecond for that many seconds after the delay, how often each
 * address in the executable was hit is written to `%TEMP%\blender_profile.txt` ("address count",
 * symbolize with the unstripped executable). A thread is only suspended while its context is
 * read, nothing is allocated meanwhile. */
static DWORD WINAPI xp_profile(LPVOID /*param*/)
{
  int delay = 0, seconds = 0;
  if (sscanf(getenv("XP_PROFILE"), "%d,%d", &delay, &seconds) != 2 || seconds <= 0) {
    return 0;
  }
  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)GetModuleHandleA(nullptr);
  const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)dos + dos->e_lfanew);
  const uintptr_t image_start = uintptr_t(dos);
  const uintptr_t image_end = image_start + nt->OptionalHeader.SizeOfImage;

  /* Open addressing on the address, counts of 0 are free slots. */
  const uint32_t table_size = 1 << 20;
  uintptr_t *addresses = (uintptr_t *)VirtualAlloc(
      nullptr, table_size * sizeof(uintptr_t), MEM_COMMIT, PAGE_READWRITE);
  uint32_t *counts = (uint32_t *)VirtualAlloc(
      nullptr, table_size * sizeof(uint32_t), MEM_COMMIT, PAGE_READWRITE);
  uint32_t outside = 0, total = 0;

  Sleep(DWORD(delay) * 1000);
  /* Preempt the threads being sampled (on one CPU they would run their whole time slice). */
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  timeBeginPeriod(1);
  HANDLE threads[64];
  int num_threads = 0;
  const DWORD end = GetTickCount() + DWORD(seconds) * 1000;
  for (int sample = 0; int(end - GetTickCount()) > 0; sample++) {
    if (sample % 200 == 0) {
      /* Threads come and go: list them again now and then. */
      for (int i = 0; i < num_threads; i++) {
        CloseHandle(threads[i]);
      }
      num_threads = 0;
      const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 entry;
      entry.dwSize = sizeof(entry);
      for (BOOL found = Thread32First(snapshot, &entry); found && num_threads < 64;
           found = Thread32Next(snapshot, &entry))
      {
        if (entry.th32OwnerProcessID == GetCurrentProcessId() &&
            entry.th32ThreadID != GetCurrentThreadId())
        {
          const HANDLE thread = OpenThread(
              THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
          if (thread != nullptr) {
            threads[num_threads++] = thread;
          }
        }
      }
      CloseHandle(snapshot);
    }
    for (int i = 0; i < num_threads; i++) {
      if (SuspendThread(threads[i]) == DWORD(-1)) {
        continue;
      }
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_CONTROL;
      const bool valid = GetThreadContext(threads[i], &context);
      ResumeThread(threads[i]);
      if (!valid) {
        continue;
      }
      const uintptr_t eip = context.Eip;
      total++;
      if (eip < image_start || eip >= image_end) {
        /* Waiting in the system (most idle threads) or running in a DLL. */
        outside++;
        continue;
      }
      uint32_t slot = uint32_t(eip * 2654435761u) & (table_size - 1);
      while (counts[slot] != 0 && addresses[slot] != eip) {
        slot = (slot + 1) & (table_size - 1);
      }
      addresses[slot] = eip;
      counts[slot]++;
    }
    Sleep(1);
  }
  timeEndPeriod(1);

  char filepath[MAX_PATH];
  GetTempPathA(MAX_PATH, filepath);
  strncat(filepath, "blender_profile.txt", MAX_PATH - strlen(filepath) - 1);
  if (FILE *file = fopen(filepath, "w")) {
    fprintf(file, "# samples %u, outside the executable %u\n", total, outside);
    for (uint32_t slot = 0; slot < table_size; slot++) {
      if (counts[slot] != 0) {
        fprintf(file, "%08lx %u\n", (unsigned long)addresses[slot], counts[slot]);
      }
    }
    fclose(file);
    fprintf(stderr, "XP_PROFILE: %u samples written to %s\n", total, filepath);
  }
  return 0;
}
#    endif

extern LONG WINAPI windows_exception_handler(EXCEPTION_POINTERS *ExceptionInfo)
{
  /* If this is a stack overflow then we can't walk the stack, so just try to show
   * where the error happened. */
  if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
    HMODULE mod;
    CHAR modulename[MAX_PATH];
    LPVOID address = ExceptionInfo->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "Error   : EXCEPTION_STACK_OVERFLOW\n");
    fprintf(stderr, "Address : 0x%p\n", address);
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, LPCSTR(address), &mod)) {
      if (GetModuleFileName(mod, modulename, MAX_PATH)) {
        fprintf(stderr, "Module  : %s\n", modulename);
      }
    }

    sig_cleanup_and_terminate(SIGSEGV);
  }
  else {
#    ifdef __MINGW32__
    {
      /* XP port: the crash may come from a corrupted heap, which the regular crash report below
       * can trip on. Print the essentials first, without allocating: the exception, and the stack
       * values pointing into the executable (a superset of the call chain). */
      char line[160];
      DWORD written;
      const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
      const EXCEPTION_RECORD *rec = ExceptionInfo->ExceptionRecord;
      int len = snprintf(line,
                         sizeof(line),
                         "Exception 0x%08lx at 0x%p, access 0x%p\nStack:",
                         (unsigned long)rec->ExceptionCode,
                         rec->ExceptionAddress,
                         rec->NumberParameters > 1 ? (void *)rec->ExceptionInformation[1] :
                                                     nullptr);
      WriteFile(err, line, DWORD(len), &written, nullptr);
      xp_print_stack_values((const uintptr_t *)ExceptionInfo->ContextRecord->Esp);
    }
#    endif
    auto crash_func = [&]() {
      char filepath_crashlog[FILE_MAX];
      BLI_windows_exception_print_message(ExceptionInfo);
      BKE_blender_globals_crash_path_get(filepath_crashlog);
      crashlog_file_generate(filepath_crashlog, ExceptionInfo);

      /* Disable popup in background mode to avoid blocking automation.
       * (e.g., when used by a render farm; see #142314). */
      if ((!G.background) && (!app_state.signal.use_console_crash_handler)) {
        std::string version;
#    ifndef BUILD_DATE
        const char *build_hash = G_MAIN ? G_MAIN->build_hash : "unknown";
        version = std::string(BKE_blender_version_string()) + ", hash: `" + build_hash + "`";
#    else
        version = std::string(BKE_blender_version_string()) +
                  ", Commit date: " + build_commit_date + " " + build_commit_time + ", hash: `" +
                  build_hash + "`";
#    endif

        BLI_windows_exception_show_dialog(
            filepath_crashlog, G.filepath_last_blend, GPU_platform_gpu_name(), version.c_str());

        sig_cleanup_and_terminate(SIGSEGV);
      }
    };

    /* A crash while handling a crash (a corrupted heap) would wait forever in `std::call_once`
     * below: the window freezes instead of closing. Give up at once. */
    static volatile LONG handler_thread = 0;
    const LONG thread = LONG(GetCurrentThreadId());
    if (InterlockedCompareExchange(&handler_thread, thread, 0) == thread) {
      fprintf(stderr, "Error   : crashed again while handling a crash, terminating\n");
      fflush(stderr);
      TerminateProcess(GetCurrentProcess(), SIGSEGV);
    }

    /* The use of `std::call_once` ensures that the crash handling function is only executed once,
     * even if multiple crashes occur simultaneously. */
    static std::once_flag crash_func_once;
    std::call_once(crash_func_once, crash_func);
  }

  return EXCEPTION_EXECUTE_HANDLER;
}
#  endif

static void sig_handle_abort(int /*signum*/)
{
  /* Delete content of temp directory. */
  BKE_tempdir_session_purge();
}

void main_signal_setup()
{
  if (app_state.signal.use_crash_handler) {
#  ifdef WIN32
    SetUnhandledExceptionFilter(windows_exception_handler);
#  else
    /* After parsing arguments. */
    signal(SIGSEGV, sig_handle_crash_fn);
#  endif
  }

#  if defined(WIN32) && defined(__MINGW32__)
  if (const char *seconds = getenv("XP_THREAD_DUMP_AFTER")) {
    CreateThread(nullptr, 0, xp_thread_dump, (LPVOID)uintptr_t(atoi(seconds)), 0, nullptr);
  }
  if (getenv("XP_PROFILE")) {
    CreateThread(nullptr, 0, xp_profile, nullptr, 0, nullptr);
  }
#  endif

#  ifdef WIN32
  /* Prevent any error mode dialogs from hanging the application. */
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOALIGNMENTFAULTEXCEPT | SEM_NOGPFAULTERRORBOX |
               SEM_NOOPENFILEERRORBOX);
#  endif

  if (app_state.signal.use_abort_handler) {
    signal(SIGABRT, sig_handle_abort);
  }
}

void main_signal_setup_background()
{
  /* for all platforms, even windows has it! */
  BLI_assert(G.background);

  /* Support pressing `Ctrl-C` to close Blender in background-mode.
   * Useful to be able to cancel a render operation. */
  signal(SIGINT, sig_handle_blender_esc);
}

void main_signal_setup_fpe()
{
#  if defined(__linux__) || defined(_WIN32) || defined(OSX_SSE_FPE)
  /* Zealous but makes float issues a heck of a lot easier to find!
   * Set breakpoints on #sig_handle_fpe. */
  signal(SIGFPE, sig_handle_fpe);

#    if defined(__linux__) && defined(__GNUC__) && defined(HAVE_FEENABLEEXCEPT)
  feenableexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW);
#    endif /* defined(__linux__) && defined(__GNUC__) */
#    if defined(OSX_SSE_FPE)
  /* OSX uses SSE for floating point by default, so here
   * use SSE instructions to throw floating point exceptions. */
  _MM_SET_EXCEPTION_MASK(_MM_MASK_MASK &
                         ~(_MM_MASK_OVERFLOW | _MM_MASK_INVALID | _MM_MASK_DIV_ZERO));
#    endif /* OSX_SSE_FPE */
#    if defined(_WIN32) && defined(_MSC_VER)
  /* Enables all floating-point exceptions. */
  _controlfp_s(nullptr, 0, _MCW_EM);
  /* Hide the ones we don't care about. */
  _controlfp_s(nullptr, _EM_DENORMAL | _EM_UNDERFLOW | _EM_INEXACT, _MCW_EM);
#    endif /* _WIN32 && _MSC_VER */
#  endif
}

}  // namespace blender

#endif /* WITH_PYTHON_MODULE */
