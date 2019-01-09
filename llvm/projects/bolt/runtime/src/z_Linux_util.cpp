/*
 * z_Linux_util.cpp -- platform specific routines.
 */

//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//

#include "kmp.h"
#include "kmp_affinity.h"
#include "kmp_i18n.h"
#include "kmp_io.h"
#include "kmp_itt.h"
#include "kmp_lock.h"
#include "kmp_stats.h"
#include "kmp_str.h"
#include "kmp_wait_release.h"
#include "kmp_wrapper_getpid.h"

#if !KMP_OS_FREEBSD && !KMP_OS_NETBSD
#include <alloca.h>
#endif
#include <math.h> // HUGE_VAL.
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>

#if KMP_OS_LINUX && !KMP_OS_CNK
#include <sys/sysinfo.h>
#if KMP_USE_FUTEX
// We should really include <futex.h>, but that causes compatibility problems on
// different Linux* OS distributions that either require that you include (or
// break when you try to include) <pci/types.h>. Since all we need is the two
// macros below (which are part of the kernel ABI, so can't change) we just
// define the constants here and don't include <futex.h>
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif
#endif
#elif KMP_OS_DARWIN
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif KMP_OS_FREEBSD
#include <pthread_np.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>

#include "tsan_annotations.h"

struct kmp_sys_timer {
  struct timespec start;
};

// Convert timespec to nanoseconds.
#define TS2NS(timespec) (((timespec).tv_sec * 1e9) + (timespec).tv_nsec)

static struct kmp_sys_timer __kmp_sys_timer_data;

#if KMP_HANDLE_SIGNALS
typedef void (*sig_func_t)(int);
STATIC_EFI2_WORKAROUND struct sigaction __kmp_sighldrs[NSIG];
static sigset_t __kmp_sigset;
#endif

static int __kmp_init_runtime = FALSE;

static int __kmp_fork_count = 0;

#if !KMP_USE_ABT
static pthread_condattr_t __kmp_suspend_cond_attr;
static pthread_mutexattr_t __kmp_suspend_mutex_attr;

static kmp_cond_align_t __kmp_wait_cv;
static kmp_mutex_align_t __kmp_wait_mx;
#endif

kmp_uint64 __kmp_ticks_per_msec = 1000000;

#ifdef DEBUG_SUSPEND
static void __kmp_print_cond(char *buffer, kmp_cond_align_t *cond) {
  KMP_SNPRINTF(buffer, 128, "(cond (lock (%ld, %d)), (descr (%p)))",
               cond->c_cond.__c_lock.__status, cond->c_cond.__c_lock.__spinlock,
               cond->c_cond.__c_waiting);
}
#endif

#if KMP_USE_ABT

static ABT_pool __kmp_abt_get_pool(int gtid);
static ABT_pool __kmp_abt_get_my_pool(int gtid);
static int __kmp_abt_sched_init(ABT_sched sched, ABT_sched_config config);
static void __kmp_abt_sched_run(ABT_sched sched);
static int __kmp_abt_sched_free(ABT_sched sched);
static void __kmp_abt_initialize(void);
static void __kmp_abt_finalize(void);

#endif

#if (KMP_OS_LINUX && KMP_AFFINITY_SUPPORTED)

/* Affinity support */

void __kmp_affinity_bind_thread(int which) {
  KMP_ASSERT2(KMP_AFFINITY_CAPABLE(),
              "Illegal set affinity operation when not capable");

  kmp_affin_mask_t *mask;
  KMP_CPU_ALLOC_ON_STACK(mask);
  KMP_CPU_ZERO(mask);
  KMP_CPU_SET(which, mask);
  __kmp_set_system_affinity(mask, TRUE);
  KMP_CPU_FREE_FROM_STACK(mask);
}

/* Determine if we can access affinity functionality on this version of
 * Linux* OS by checking __NR_sched_{get,set}affinity system calls, and set
 * __kmp_affin_mask_size to the appropriate value (0 means not capable). */
void __kmp_affinity_determine_capable(const char *env_var) {
// Check and see if the OS supports thread affinity.

#define KMP_CPU_SET_SIZE_LIMIT (1024 * 1024)

  int gCode;
  int sCode;
  unsigned char *buf;
  buf = (unsigned char *)KMP_INTERNAL_MALLOC(KMP_CPU_SET_SIZE_LIMIT);

  // If Linux* OS:
  // If the syscall fails or returns a suggestion for the size,
  // then we don't have to search for an appropriate size.
  gCode = syscall(__NR_sched_getaffinity, 0, KMP_CPU_SET_SIZE_LIMIT, buf);
  KA_TRACE(30, ("__kmp_affinity_determine_capable: "
                "initial getaffinity call returned %d errno = %d\n",
                gCode, errno));

  // if ((gCode < 0) && (errno == ENOSYS))
  if (gCode < 0) {
    // System call not supported
    if (__kmp_affinity_verbose ||
        (__kmp_affinity_warnings && (__kmp_affinity_type != affinity_none) &&
         (__kmp_affinity_type != affinity_default) &&
         (__kmp_affinity_type != affinity_disabled))) {
      int error = errno;
      kmp_msg_t err_code = KMP_ERR(error);
      __kmp_msg(kmp_ms_warning, KMP_MSG(GetAffSysCallNotSupported, env_var),
                err_code, __kmp_msg_null);
      if (__kmp_generate_warnings == kmp_warnings_off) {
        __kmp_str_free(&err_code.str);
      }
    }
    KMP_AFFINITY_DISABLE();
    KMP_INTERNAL_FREE(buf);
    return;
  }
  if (gCode > 0) { // Linux* OS only
    // The optimal situation: the OS returns the size of the buffer it expects.
    //
    // A verification of correct behavior is that Isetaffinity on a NULL
    // buffer with the same size fails with errno set to EFAULT.
    sCode = syscall(__NR_sched_setaffinity, 0, gCode, NULL);
    KA_TRACE(30, ("__kmp_affinity_determine_capable: "
                  "setaffinity for mask size %d returned %d errno = %d\n",
                  gCode, sCode, errno));
    if (sCode < 0) {
      if (errno == ENOSYS) {
        if (__kmp_affinity_verbose ||
            (__kmp_affinity_warnings &&
             (__kmp_affinity_type != affinity_none) &&
             (__kmp_affinity_type != affinity_default) &&
             (__kmp_affinity_type != affinity_disabled))) {
          int error = errno;
          kmp_msg_t err_code = KMP_ERR(error);
          __kmp_msg(kmp_ms_warning, KMP_MSG(SetAffSysCallNotSupported, env_var),
                    err_code, __kmp_msg_null);
          if (__kmp_generate_warnings == kmp_warnings_off) {
            __kmp_str_free(&err_code.str);
          }
        }
        KMP_AFFINITY_DISABLE();
        KMP_INTERNAL_FREE(buf);
      }
      if (errno == EFAULT) {
        KMP_AFFINITY_ENABLE(gCode);
        KA_TRACE(10, ("__kmp_affinity_determine_capable: "
                      "affinity supported (mask size %d)\n",
                      (int)__kmp_affin_mask_size));
        KMP_INTERNAL_FREE(buf);
        return;
      }
    }
  }

  // Call the getaffinity system call repeatedly with increasing set sizes
  // until we succeed, or reach an upper bound on the search.
  KA_TRACE(30, ("__kmp_affinity_determine_capable: "
                "searching for proper set size\n"));
  int size;
  for (size = 1; size <= KMP_CPU_SET_SIZE_LIMIT; size *= 2) {
    gCode = syscall(__NR_sched_getaffinity, 0, size, buf);
    KA_TRACE(30, ("__kmp_affinity_determine_capable: "
                  "getaffinity for mask size %d returned %d errno = %d\n",
                  size, gCode, errno));

    if (gCode < 0) {
      if (errno == ENOSYS) {
        // We shouldn't get here
        KA_TRACE(30, ("__kmp_affinity_determine_capable: "
                      "inconsistent OS call behavior: errno == ENOSYS for mask "
                      "size %d\n",
                      size));
        if (__kmp_affinity_verbose ||
            (__kmp_affinity_warnings &&
             (__kmp_affinity_type != affinity_none) &&
             (__kmp_affinity_type != affinity_default) &&
             (__kmp_affinity_type != affinity_disabled))) {
          int error = errno;
          kmp_msg_t err_code = KMP_ERR(error);
          __kmp_msg(kmp_ms_warning, KMP_MSG(GetAffSysCallNotSupported, env_var),
                    err_code, __kmp_msg_null);
          if (__kmp_generate_warnings == kmp_warnings_off) {
            __kmp_str_free(&err_code.str);
          }
        }
        KMP_AFFINITY_DISABLE();
        KMP_INTERNAL_FREE(buf);
        return;
      }
      continue;
    }

    sCode = syscall(__NR_sched_setaffinity, 0, gCode, NULL);
    KA_TRACE(30, ("__kmp_affinity_determine_capable: "
                  "setaffinity for mask size %d returned %d errno = %d\n",
                  gCode, sCode, errno));
    if (sCode < 0) {
      if (errno == ENOSYS) { // Linux* OS only
        // We shouldn't get here
        KA_TRACE(30, ("__kmp_affinity_determine_capable: "
                      "inconsistent OS call behavior: errno == ENOSYS for mask "
                      "size %d\n",
                      size));
        if (__kmp_affinity_verbose ||
            (__kmp_affinity_warnings &&
             (__kmp_affinity_type != affinity_none) &&
             (__kmp_affinity_type != affinity_default) &&
             (__kmp_affinity_type != affinity_disabled))) {
          int error = errno;
          kmp_msg_t err_code = KMP_ERR(error);
          __kmp_msg(kmp_ms_warning, KMP_MSG(SetAffSysCallNotSupported, env_var),
                    err_code, __kmp_msg_null);
          if (__kmp_generate_warnings == kmp_warnings_off) {
            __kmp_str_free(&err_code.str);
          }
        }
        KMP_AFFINITY_DISABLE();
        KMP_INTERNAL_FREE(buf);
        return;
      }
      if (errno == EFAULT) {
        KMP_AFFINITY_ENABLE(gCode);
        KA_TRACE(10, ("__kmp_affinity_determine_capable: "
                      "affinity supported (mask size %d)\n",
                      (int)__kmp_affin_mask_size));
        KMP_INTERNAL_FREE(buf);
        return;
      }
    }
  }
  // save uncaught error code
  // int error = errno;
  KMP_INTERNAL_FREE(buf);
  // restore uncaught error code, will be printed at the next KMP_WARNING below
  // errno = error;

  // Affinity is not supported
  KMP_AFFINITY_DISABLE();
  KA_TRACE(10, ("__kmp_affinity_determine_capable: "
                "cannot determine mask size - affinity not supported\n"));
  if (__kmp_affinity_verbose ||
      (__kmp_affinity_warnings && (__kmp_affinity_type != affinity_none) &&
       (__kmp_affinity_type != affinity_default) &&
       (__kmp_affinity_type != affinity_disabled))) {
    KMP_WARNING(AffCantGetMaskSize, env_var);
  }
}

#endif // KMP_OS_LINUX && KMP_AFFINITY_SUPPORTED

#if KMP_USE_FUTEX

int __kmp_futex_determine_capable() {
#if KMP_USE_ABT
  return 0; // Not supported.
#else
  int loc = 0;
  int rc = syscall(__NR_futex, &loc, FUTEX_WAKE, 1, NULL, NULL, 0);
  int retval = (rc == 0) || (errno != ENOSYS);

  KA_TRACE(10,
           ("__kmp_futex_determine_capable: rc = %d errno = %d\n", rc, errno));
  KA_TRACE(10, ("__kmp_futex_determine_capable: futex syscall%s supported\n",
                retval ? "" : " not"));

  return retval;
#endif
}

#endif // KMP_USE_FUTEX

#if (KMP_ARCH_X86 || KMP_ARCH_X86_64) && (!KMP_ASM_INTRINS)
/* Only 32-bit "add-exchange" instruction on IA-32 architecture causes us to
   use compare_and_store for these routines */

kmp_int8 __kmp_test_then_or8(volatile kmp_int8 *p, kmp_int8 d) {
  kmp_int8 old_value, new_value;

  old_value = TCR_1(*p);
  new_value = old_value | d;

  while (!KMP_COMPARE_AND_STORE_REL8(p, old_value, new_value)) {
    KMP_CPU_PAUSE();
    old_value = TCR_1(*p);
    new_value = old_value | d;
  }
  return old_value;
}

kmp_int8 __kmp_test_then_and8(volatile kmp_int8 *p, kmp_int8 d) {
  kmp_int8 old_value, new_value;

  old_value = TCR_1(*p);
  new_value = old_value & d;

  while (!KMP_COMPARE_AND_STORE_REL8(p, old_value, new_value)) {
    KMP_CPU_PAUSE();
    old_value = TCR_1(*p);
    new_value = old_value & d;
  }
  return old_value;
}

kmp_uint32 __kmp_test_then_or32(volatile kmp_uint32 *p, kmp_uint32 d) {
  kmp_uint32 old_value, new_value;

  old_value = TCR_4(*p);
  new_value = old_value | d;

  while (!KMP_COMPARE_AND_STORE_REL32(p, old_value, new_value)) {
    KMP_CPU_PAUSE();
    old_value = TCR_4(*p);
    new_value = old_value | d;
  }
  return old_value;
}

kmp_uint32 __kmp_test_then_and32(volatile kmp_uint32 *p, kmp_uint32 d) {
  kmp_uint32 old_value, new_value;

  old_value = TCR_4(*p);
  new_value = old_value & d;

  while (!KMP_COMPARE_AND_STORE_REL32(p, old_value, new_value)) {
    KMP_CPU_PAUSE();
    old_value = TCR_4(*p);
    new_value = old_value & d;
  }
  return old_value;
}

#if KMP_ARCH_X86
kmp_int8 __kmp_test_then_add8(volatile kmp_int8 *p, kmp_int8 d) {
  kmp_int8 old_value, new_value;

  old_value = TCR_1(*p);
  new_value = old_value + d;

  while (!KMP_COMPARE_AND_STORE_REL8(p, old_value, new_value)) {
    KMP_CPU_PAUSE();
    old_value = TCR_1(*p);
    new_value = old_value + d;
  }
  return old_value;
}

kmp_int64 __kmp_test_then_add64(volatile kmp_int64 *p, kmp_int64 d) {
  kmp_int64 old_value, new_value;

  old_value = TCR_8(*p);
  new_value = old_value + d;

  while (!KMP_COMPARE_AND_STORE_REL64(p, old_value, new_value)) {
    KMP_CPU_PAUSE();
    old_value = TCR_8(*p);
    new_value = old_value + d;
  }
  return old_value;
}
#endif /* KMP_ARCH_X86 */

kmp_uint64 __kmp_test_then_or64(volatile kmp_uint64 *p, kmp_uint64 d) {
  kmp_uint64 old_value, new_value;

  old_value = TCR_8(*p);
  new_value = old_value | d;
  while (!KMP_COMPARE_AND_STORE_REL64(p, old_value, new_value)) {
    KMP_CPU_PAUSE();
    old_value = TCR_8(*p);
    new_value = old_value | d;
  }
  return old_value;
}

kmp_uint64 __kmp_test_then_and64(volatile kmp_uint64 *p, kmp_uint64 d) {
  kmp_uint64 old_value, new_value;

  old_value = TCR_8(*p);
  new_value = old_value & d;
  while (!KMP_COMPARE_AND_STORE_REL64(p, old_value, new_value)) {
    KMP_CPU_PAUSE();
    old_value = TCR_8(*p);
    new_value = old_value & d;
  }
  return old_value;
}

#endif /* (KMP_ARCH_X86 || KMP_ARCH_X86_64) && (! KMP_ASM_INTRINS) */

void __kmp_terminate_thread(int gtid) {
  int status;
  kmp_info_t *th = __kmp_threads[gtid];

  if (!th)
    return;

#ifdef KMP_CANCEL_THREADS
  KA_TRACE(10, ("__kmp_terminate_thread: kill (%d)\n", gtid));
#if KMP_USE_ABT
  status = ABT_thread_cancel(th->th.th_info.ds.ds_thread);
  if (status != ABT_SUCCESS) {
    __kmp_fatal(KMP_MSG(CantTerminateWorkerThread), KMP_ERR(status),
                __kmp_msg_null);
  }
#else // KMP_USE_ABT
  status = pthread_cancel(th->th.th_info.ds.ds_thread);
  if (status != 0 && status != ESRCH) {
    __kmp_fatal(KMP_MSG(CantTerminateWorkerThread), KMP_ERR(status),
                __kmp_msg_null);
  }
#endif // !KMP_USE_ABT
#endif
  __kmp_yield(TRUE);
} //

/* Set thread stack info according to values returned by pthread_getattr_np().
   If values are unreasonable, assume call failed and use incremental stack
   refinement method instead. Returns TRUE if the stack parameters could be
   determined exactly, FALSE if incremental refinement is necessary. */
static kmp_int32 __kmp_set_stack_info(int gtid, kmp_info_t *th) {
  int stack_data;
#if KMP_OS_LINUX || KMP_OS_FREEBSD || KMP_OS_NETBSD
  /* Linux* OS only -- no pthread_getattr_np support on OS X* */
  pthread_attr_t attr;
  int status;
  size_t size = 0;
  void *addr = 0;

  /* Always do incremental stack refinement for ubermaster threads since the
     initial thread stack range can be reduced by sibling thread creation so
     pthread_attr_getstack may cause thread gtid aliasing */
  if (!KMP_UBER_GTID(gtid)) {

    /* Fetch the real thread attributes */
    status = pthread_attr_init(&attr);
    KMP_CHECK_SYSFAIL("pthread_attr_init", status);
#if KMP_OS_FREEBSD || KMP_OS_NETBSD
    status = pthread_attr_get_np(pthread_self(), &attr);
    KMP_CHECK_SYSFAIL("pthread_attr_get_np", status);
#else
    status = pthread_getattr_np(pthread_self(), &attr);
    KMP_CHECK_SYSFAIL("pthread_getattr_np", status);
#endif
    status = pthread_attr_getstack(&attr, &addr, &size);
    KMP_CHECK_SYSFAIL("pthread_attr_getstack", status);
    KA_TRACE(60,
             ("__kmp_set_stack_info: T#%d pthread_attr_getstack returned size:"
              " %lu, low addr: %p\n",
              gtid, size, addr));
    status = pthread_attr_destroy(&attr);
    KMP_CHECK_SYSFAIL("pthread_attr_destroy", status);
  }

  if (size != 0 && addr != 0) { // was stack parameter determination successful?
    /* Store the correct base and size */
    TCW_PTR(th->th.th_info.ds.ds_stackbase, (((char *)addr) + size));
    TCW_PTR(th->th.th_info.ds.ds_stacksize, size);
    TCW_4(th->th.th_info.ds.ds_stackgrow, FALSE);
    return TRUE;
  }
#endif /* KMP_OS_LINUX || KMP_OS_FREEBSD || KMP_OS_NETBSD */
  /* Use incremental refinement starting from initial conservative estimate */
  TCW_PTR(th->th.th_info.ds.ds_stacksize, 0);
  TCW_PTR(th->th.th_info.ds.ds_stackbase, &stack_data);
  TCW_4(th->th.th_info.ds.ds_stackgrow, TRUE);
  return FALSE;
}

#if !KMP_USE_ABT

static void *__kmp_launch_worker(void *thr) {
  int status, old_type, old_state;
#ifdef KMP_BLOCK_SIGNALS
  sigset_t new_set, old_set;
#endif /* KMP_BLOCK_SIGNALS */
  void *exit_val;
#if KMP_OS_LINUX || KMP_OS_FREEBSD || KMP_OS_NETBSD
  void *volatile padding = 0;
#endif
  int gtid;

  gtid = ((kmp_info_t *)thr)->th.th_info.ds.ds_gtid;
  __kmp_gtid_set_specific(gtid);
#ifdef KMP_TDATA_GTID
  __kmp_gtid = gtid;
#endif
#if KMP_STATS_ENABLED
  // set thread local index to point to thread-specific stats
  __kmp_stats_thread_ptr = ((kmp_info_t *)thr)->th.th_stats;
  __kmp_stats_thread_ptr->startLife();
  KMP_SET_THREAD_STATE(IDLE);
  KMP_INIT_PARTITIONED_TIMERS(OMP_idle);
#endif

#if USE_ITT_BUILD
  __kmp_itt_thread_name(gtid);
#endif /* USE_ITT_BUILD */

#if KMP_AFFINITY_SUPPORTED
  __kmp_affinity_set_init_mask(gtid, FALSE);
#endif

#ifdef KMP_CANCEL_THREADS
  status = pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old_type);
  KMP_CHECK_SYSFAIL("pthread_setcanceltype", status);
  // josh todo: isn't PTHREAD_CANCEL_ENABLE default for newly-created threads?
  status = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
  KMP_CHECK_SYSFAIL("pthread_setcancelstate", status);
#endif

#if KMP_ARCH_X86 || KMP_ARCH_X86_64
  // Set FP control regs to be a copy of the parallel initialization thread's.
  __kmp_clear_x87_fpu_status_word();
  __kmp_load_x87_fpu_control_word(&__kmp_init_x87_fpu_control_word);
  __kmp_load_mxcsr(&__kmp_init_mxcsr);
#endif /* KMP_ARCH_X86 || KMP_ARCH_X86_64 */

#ifdef KMP_BLOCK_SIGNALS
  status = sigfillset(&new_set);
  KMP_CHECK_SYSFAIL_ERRNO("sigfillset", status);
  status = pthread_sigmask(SIG_BLOCK, &new_set, &old_set);
  KMP_CHECK_SYSFAIL("pthread_sigmask", status);
#endif /* KMP_BLOCK_SIGNALS */

#if KMP_OS_LINUX || KMP_OS_FREEBSD || KMP_OS_NETBSD
  if (__kmp_stkoffset > 0 && gtid > 0) {
    padding = KMP_ALLOCA(gtid * __kmp_stkoffset);
  }
#endif

  KMP_MB();
  __kmp_set_stack_info(gtid, (kmp_info_t *)thr);

  __kmp_check_stack_overlap((kmp_info_t *)thr);

  exit_val = __kmp_launch_thread((kmp_info_t *)thr);

#ifdef KMP_BLOCK_SIGNALS
  status = pthread_sigmask(SIG_SETMASK, &old_set, NULL);
  KMP_CHECK_SYSFAIL("pthread_sigmask", status);
#endif /* KMP_BLOCK_SIGNALS */

  return exit_val;
}

#else // !KMP_USE_ABT

static void __kmp_abt_launch_worker(void *thr) {
  int status, old_type, old_state;
  int gtid;
  kmp_info_t *this_thr = (kmp_info_t *)thr;
  kmp_team_t *(*volatile pteam);

  gtid = this_thr->th.th_info.ds.ds_gtid;
  KMP_DEBUG_ASSERT(this_thr == __kmp_threads[gtid]);

#if KMP_AFFINITY_SUPPORTED
  __kmp_affinity_set_init_mask(gtid, FALSE);
#endif

  KMP_MB();

  pteam = (kmp_team_t *(*))(& this_thr->th.th_team);
  if (__kmp_tasking_mode != tskm_immediate_exec) {
    /* It is originally set up in task_team_sync() */
    this_thr->th.th_task_team = (*pteam)->t.t_task_team
                                [this_thr->th.th_task_state];
  }
  if (TCR_SYNC_PTR(*pteam) && !TCR_4(__kmp_global.g.g_done)) {
    /* run our new task */
    if (TCR_SYNC_PTR((*pteam)->t.t_pkfn) != NULL) {
      int rc;
      KA_TRACE(20, ("__kmp_abt_launch_worker: T#%d(%d:%d) "
                    "invoke microtask = %p\n",
                    gtid, (*pteam)->t.t_id, __kmp_tid_from_gtid(gtid),
                    (*pteam)->t.t_pkfn));
      KMP_STOP_DEVELOPER_EXPLICIT_TIMER(USER_launch_thread_loop);
      {
          KMP_TIME_DEVELOPER_BLOCK(USER_worker_invoke);
          rc = (*pteam)->t.t_invoke(gtid);
      }
      KMP_START_DEVELOPER_EXPLICIT_TIMER(USER_launch_thread_loop);
      KMP_ASSERT(rc);
      KMP_MB();
      KA_TRACE(20, ("__kmp_abt_launch_worker: T#%d(%d:%d) "
                    "done microtask = %p\n",
                    gtid, (*pteam)->t.t_id, __kmp_tid_from_gtid(gtid),
                    (*pteam)->t.t_pkfn));
    }
  }

  KA_TRACE(10, ("__kmp_abt_launch_worker: T#%d done\n", gtid));

  __kmp_abt_wait_child_tasks(this_thr, FALSE);
  this_thr->th.th_task_team = NULL;

  /* Below is for the implicit task */
  kmp_taskdata_t *td = this_thr->th.th_current_task;
  if (td->td_task_queue) {
    KMP_DEBUG_ASSERT(td->td_tq_cur_size == 0);
    KMP_INTERNAL_FREE(td->td_task_queue);
    td->td_task_queue = NULL;
    td->td_tq_max_size = 0;
  }
}

#endif // KMP_USE_ABT

#if KMP_USE_MONITOR
/* The monitor thread controls all of the threads in the complex */

static void *__kmp_launch_monitor(void *thr) {
  int status, old_type, old_state;
#ifdef KMP_BLOCK_SIGNALS
  sigset_t new_set;
#endif /* KMP_BLOCK_SIGNALS */
  struct timespec interval;
  int yield_count;
  int yield_cycles = 0;

  KMP_MB(); /* Flush all pending memory write invalidates.  */

  KA_TRACE(10, ("__kmp_launch_monitor: #1 launched\n"));

  /* register us as the monitor thread */
  __kmp_gtid_set_specific(KMP_GTID_MONITOR);
#ifdef KMP_TDATA_GTID
  __kmp_gtid = KMP_GTID_MONITOR;
#endif

  KMP_MB();

#if USE_ITT_BUILD
  // Instruct Intel(R) Threading Tools to ignore monitor thread.
  __kmp_itt_thread_ignore();
#endif /* USE_ITT_BUILD */

  __kmp_set_stack_info(((kmp_info_t *)thr)->th.th_info.ds.ds_gtid,
                       (kmp_info_t *)thr);

  __kmp_check_stack_overlap((kmp_info_t *)thr);

#ifdef KMP_CANCEL_THREADS
  status = pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old_type);
  KMP_CHECK_SYSFAIL("pthread_setcanceltype", status);
  // josh todo: isn't PTHREAD_CANCEL_ENABLE default for newly-created threads?
  status = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
  KMP_CHECK_SYSFAIL("pthread_setcancelstate", status);
#endif

#if KMP_REAL_TIME_FIX
  // This is a potential fix which allows application with real-time scheduling
  // policy work. However, decision about the fix is not made yet, so it is
  // disabled by default.
  { // Are program started with real-time scheduling policy?
    int sched = sched_getscheduler(0);
    if (sched == SCHED_FIFO || sched == SCHED_RR) {
      // Yes, we are a part of real-time application. Try to increase the
      // priority of the monitor.
      struct sched_param param;
      int max_priority = sched_get_priority_max(sched);
      int rc;
      KMP_WARNING(RealTimeSchedNotSupported);
      sched_getparam(0, &param);
      if (param.sched_priority < max_priority) {
        param.sched_priority += 1;
        rc = sched_setscheduler(0, sched, &param);
        if (rc != 0) {
          int error = errno;
          kmp_msg_t err_code = KMP_ERR(error);
          __kmp_msg(kmp_ms_warning, KMP_MSG(CantChangeMonitorPriority),
                    err_code, KMP_MSG(MonitorWillStarve), __kmp_msg_null);
          if (__kmp_generate_warnings == kmp_warnings_off) {
            __kmp_str_free(&err_code.str);
          }
        }
      } else {
        // We cannot abort here, because number of CPUs may be enough for all
        // the threads, including the monitor thread, so application could
        // potentially work...
        __kmp_msg(kmp_ms_warning, KMP_MSG(RunningAtMaxPriority),
                  KMP_MSG(MonitorWillStarve), KMP_HNT(RunningAtMaxPriority),
                  __kmp_msg_null);
      }
    }
    // AC: free thread that waits for monitor started
    TCW_4(__kmp_global.g.g_time.dt.t_value, 0);
  }
#endif // KMP_REAL_TIME_FIX

  KMP_MB(); /* Flush all pending memory write invalidates.  */

  if (__kmp_monitor_wakeups == 1) {
    interval.tv_sec = 1;
    interval.tv_nsec = 0;
  } else {
    interval.tv_sec = 0;
    interval.tv_nsec = (KMP_NSEC_PER_SEC / __kmp_monitor_wakeups);
  }

  KA_TRACE(10, ("__kmp_launch_monitor: #2 monitor\n"));

  if (__kmp_yield_cycle) {
    __kmp_yielding_on = 0; /* Start out with yielding shut off */
    yield_count = __kmp_yield_off_count;
  } else {
    __kmp_yielding_on = 1; /* Yielding is on permanently */
  }

  while (!TCR_4(__kmp_global.g.g_done)) {
    struct timespec now;
    struct timeval tval;

    /*  This thread monitors the state of the system */

    KA_TRACE(15, ("__kmp_launch_monitor: update\n"));

    status = gettimeofday(&tval, NULL);
    KMP_CHECK_SYSFAIL_ERRNO("gettimeofday", status);
    TIMEVAL_TO_TIMESPEC(&tval, &now);

    now.tv_sec += interval.tv_sec;
    now.tv_nsec += interval.tv_nsec;

    if (now.tv_nsec >= KMP_NSEC_PER_SEC) {
      now.tv_sec += 1;
      now.tv_nsec -= KMP_NSEC_PER_SEC;
    }

    status = pthread_mutex_lock(&__kmp_wait_mx.m_mutex);
    KMP_CHECK_SYSFAIL("pthread_mutex_lock", status);
    // AC: the monitor should not fall asleep if g_done has been set
    if (!TCR_4(__kmp_global.g.g_done)) { // check once more under mutex
      status = pthread_cond_timedwait(&__kmp_wait_cv.c_cond,
                                      &__kmp_wait_mx.m_mutex, &now);
      if (status != 0) {
        if (status != ETIMEDOUT && status != EINTR) {
          KMP_SYSFAIL("pthread_cond_timedwait", status);
        }
      }
    }
    status = pthread_mutex_unlock(&__kmp_wait_mx.m_mutex);
    KMP_CHECK_SYSFAIL("pthread_mutex_unlock", status);

    if (__kmp_yield_cycle) {
      yield_cycles++;
      if ((yield_cycles % yield_count) == 0) {
        if (__kmp_yielding_on) {
          __kmp_yielding_on = 0; /* Turn it off now */
          yield_count = __kmp_yield_off_count;
        } else {
          __kmp_yielding_on = 1; /* Turn it on now */
          yield_count = __kmp_yield_on_count;
        }
        yield_cycles = 0;
      }
    } else {
      __kmp_yielding_on = 1;
    }

    TCW_4(__kmp_global.g.g_time.dt.t_value,
          TCR_4(__kmp_global.g.g_time.dt.t_value) + 1);

    KMP_MB(); /* Flush all pending memory write invalidates.  */
  }

  KA_TRACE(10, ("__kmp_launch_monitor: #3 cleanup\n"));

#ifdef KMP_BLOCK_SIGNALS
  status = sigfillset(&new_set);
  KMP_CHECK_SYSFAIL_ERRNO("sigfillset", status);
  status = pthread_sigmask(SIG_UNBLOCK, &new_set, NULL);
  KMP_CHECK_SYSFAIL("pthread_sigmask", status);
#endif /* KMP_BLOCK_SIGNALS */

  KA_TRACE(10, ("__kmp_launch_monitor: #4 finished\n"));

  if (__kmp_global.g.g_abort != 0) {
    /* now we need to terminate the worker threads  */
    /* the value of t_abort is the signal we caught */

    int gtid;

    KA_TRACE(10, ("__kmp_launch_monitor: #5 terminate sig=%d\n",
                  __kmp_global.g.g_abort));

    /* terminate the OpenMP worker threads */
    /* TODO this is not valid for sibling threads!!
     * the uber master might not be 0 anymore.. */
    for (gtid = 1; gtid < __kmp_threads_capacity; ++gtid)
      __kmp_terminate_thread(gtid);

    __kmp_cleanup();

    KA_TRACE(10, ("__kmp_launch_monitor: #6 raise sig=%d\n",
                  __kmp_global.g.g_abort));

    if (__kmp_global.g.g_abort > 0)
      raise(__kmp_global.g.g_abort);
  }

  KA_TRACE(10, ("__kmp_launch_monitor: #7 exit\n"));

  return thr;
}
#endif // KMP_USE_MONITOR

void __kmp_create_worker(int gtid, kmp_info_t *th, size_t stack_size) {
#if KMP_USE_ABT
  ABT_thread handle;
  ABT_thread_attr thread_attr = ABT_THREAD_ATTR_NULL;
  ABT_pool tar_pool;
  int status;
#else
  pthread_t handle;
  pthread_attr_t thread_attr;
  int status;
#endif

#if KMP_USE_ABT
  // [SM] th->th.th_info.ds.ds_gtid is setup in __kmp_allocate_thread
  KMP_DEBUG_ASSERT(th->th.th_info.ds.ds_gtid == gtid);
#else
  th->th.th_info.ds.ds_gtid = gtid;
#endif

#if KMP_STATS_ENABLED
  // sets up worker thread stats
  __kmp_acquire_tas_lock(&__kmp_stats_lock, gtid);

  // th->th.th_stats is used to transfer thread-specific stats-pointer to
  // __kmp_launch_worker. So when thread is created (goes into
  // __kmp_launch_worker) it will set its thread local pointer to
  // th->th.th_stats
  if (!KMP_UBER_GTID(gtid)) {
    th->th.th_stats = __kmp_stats_list->push_back(gtid);
  } else {
    // For root threads, __kmp_stats_thread_ptr is set in __kmp_register_root(),
    // so set the th->th.th_stats field to it.
    th->th.th_stats = __kmp_stats_thread_ptr;
  }
  __kmp_release_tas_lock(&__kmp_stats_lock, gtid);

#endif // KMP_STATS_ENABLED

#if KMP_USE_ABT
  // uber thread is created in __kmp_abt_create_uber().
  KMP_DEBUG_ASSERT(!KMP_UBER_GTID(gtid));
#else
  if (KMP_UBER_GTID(gtid)) {
    KA_TRACE(10, ("__kmp_create_worker: uber thread (%d)\n", gtid));
    th->th.th_info.ds.ds_thread = pthread_self();
    __kmp_set_stack_info(gtid, th);
    __kmp_check_stack_overlap(th);
    return;
  }
#endif

  KA_TRACE(10, ("__kmp_create_worker: try to create thread (%d)\n", gtid));

  KMP_MB(); /* Flush all pending memory write invalidates.  */

#if KMP_USE_ABT

  // If this new thread is for nested parallel region, the new thread is
  // added to the shared pool of ES where the caller thread is running on.
  if (th->th.th_team->t.t_level > 1) {
    tar_pool = __kmp_abt_get_my_pool(gtid);
  } else {
    tar_pool = __kmp_abt_get_pool(gtid);
  }

  // Check if we can revive this thread.
  if (th->th.th_info.ds.ds_thread != ABT_THREAD_NULL) {
    // Revive it.
    status = ABT_thread_revive(tar_pool, __kmp_abt_launch_worker, (void *)th,
                               &th->th.th_info.ds.ds_thread);
    KMP_ASSERT(status == ABT_SUCCESS);
    return;
  }

#endif

#ifdef KMP_THREAD_ATTR
#if KMP_USE_ABT

  status = ABT_thread_attr_create(&thread_attr);
  if (status != ABT_SUCCESS) {
    __kmp_msg(kmp_ms_fatal, KMP_MSG(CantInitThreadAttrs), KMP_ERR(status),
              __kmp_msg_null);
  }
  status = ABT_thread_attr_set_stacksize(thread_attr, stack_size);
  if (status != ABT_SUCCESS) {
    __kmp_msg(kmp_ms_fatal, KMP_MSG(CantSetWorkerStackSize, stack_size),
              KMP_ERR(status), KMP_HNT(ChangeWorkerStackSize),
              __kmp_msg_null);
  }

#else /* KMP_USE_ABT */

  status = pthread_attr_init(&thread_attr);
  if (status != 0) {
    __kmp_fatal(KMP_MSG(CantInitThreadAttrs), KMP_ERR(status), __kmp_msg_null);
  }
  status = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
  if (status != 0) {
    __kmp_fatal(KMP_MSG(CantSetWorkerState), KMP_ERR(status), __kmp_msg_null);
  }

  /* Set stack size for this thread now.
     The multiple of 2 is there because on some machines, requesting an unusual
     stacksize causes the thread to have an offset before the dummy alloca()
     takes place to create the offset.  Since we want the user to have a
     sufficient stacksize AND support a stack offset, we alloca() twice the
     offset so that the upcoming alloca() does not eliminate any premade offset,
     and also gives the user the stack space they requested for all threads */
  stack_size += gtid * __kmp_stkoffset * 2;

  KA_TRACE(10, ("__kmp_create_worker: T#%d, default stacksize = %lu bytes, "
                "__kmp_stksize = %lu bytes, final stacksize = %lu bytes\n",
                gtid, KMP_DEFAULT_STKSIZE, __kmp_stksize, stack_size));

#ifdef _POSIX_THREAD_ATTR_STACKSIZE
  status = pthread_attr_setstacksize(&thread_attr, stack_size);
#ifdef KMP_BACKUP_STKSIZE
  if (status != 0) {
    if (!__kmp_env_stksize) {
      stack_size = KMP_BACKUP_STKSIZE + gtid * __kmp_stkoffset;
      __kmp_stksize = KMP_BACKUP_STKSIZE;
      KA_TRACE(10, ("__kmp_create_worker: T#%d, default stacksize = %lu bytes, "
                    "__kmp_stksize = %lu bytes, (backup) final stacksize = %lu "
                    "bytes\n",
                    gtid, KMP_DEFAULT_STKSIZE, __kmp_stksize, stack_size));
      status = pthread_attr_setstacksize(&thread_attr, stack_size);
    }
  }
#endif /* KMP_BACKUP_STKSIZE */
  if (status != 0) {
    __kmp_fatal(KMP_MSG(CantSetWorkerStackSize, stack_size), KMP_ERR(status),
                KMP_HNT(ChangeWorkerStackSize), __kmp_msg_null);
  }
#endif /* _POSIX_THREAD_ATTR_STACKSIZE */

#endif /* !KMP_USE_ABT */

#endif /* KMP_THREAD_ATTR */

#if KMP_USE_ABT

  KA_TRACE(10, ("__kmp_create_worker: T#%d, nesting level=%d, "
                "target pool=%p\n", gtid, th->th.th_team->t.t_level, tar_pool));
  status = ABT_thread_create(tar_pool, __kmp_abt_launch_worker, (void *)th,
                             thread_attr, &handle);
  KMP_ASSERT(status == ABT_SUCCESS);

#else /* KMP_USE_ABT */

  status =
      pthread_create(&handle, &thread_attr, __kmp_launch_worker, (void *)th);
  if (status != 0 || !handle) { // ??? Why do we check handle??
#ifdef _POSIX_THREAD_ATTR_STACKSIZE
    if (status == EINVAL) {
      __kmp_fatal(KMP_MSG(CantSetWorkerStackSize, stack_size), KMP_ERR(status),
                  KMP_HNT(IncreaseWorkerStackSize), __kmp_msg_null);
    }
    if (status == ENOMEM) {
      __kmp_fatal(KMP_MSG(CantSetWorkerStackSize, stack_size), KMP_ERR(status),
                  KMP_HNT(DecreaseWorkerStackSize), __kmp_msg_null);
    }
#endif /* _POSIX_THREAD_ATTR_STACKSIZE */
    if (status == EAGAIN) {
      __kmp_fatal(KMP_MSG(NoResourcesForWorkerThread), KMP_ERR(status),
                  KMP_HNT(Decrease_NUM_THREADS), __kmp_msg_null);
    }
    KMP_SYSFAIL("pthread_create", status);
  }

#endif /* !KMP_USE_ABT */

  th->th.th_info.ds.ds_thread = handle;

#ifdef KMP_THREAD_ATTR
#if KMP_USE_ABT
  status = ABT_thread_attr_free(&thread_attr);
  KMP_ASSERT(status == ABT_SUCCESS);
#else /* KMP_USE_ABT */
  status = pthread_attr_destroy(&thread_attr);
  if (status) {
    kmp_msg_t err_code = KMP_ERR(status);
    __kmp_msg(kmp_ms_warning, KMP_MSG(CantDestroyThreadAttrs), err_code,
              __kmp_msg_null);
    if (__kmp_generate_warnings == kmp_warnings_off) {
      __kmp_str_free(&err_code.str);
    }
  }
#endif /* !KMP_USE_ABT */
#endif /* KMP_THREAD_ATTR */

  KMP_MB(); /* Flush all pending memory write invalidates.  */

  KA_TRACE(10, ("__kmp_create_worker: done creating thread (%d)\n", gtid));

} // __kmp_create_worker

#if KMP_USE_ABT

void __kmp_abt_join_worker(kmp_info_t *th) {
  int status;

  KMP_MB(); /* Flush all pending memory write invalidates.  */

  KA_TRACE(10, ("__kmp_abt_join_worker: try to join worker T#%d\n",
                th->th.th_info.ds.ds_gtid) );

  ABT_thread ds_thread = th->th.th_info.ds.ds_thread;
  status = ABT_thread_join(ds_thread);
  KMP_ASSERT(status == ABT_SUCCESS);

  KA_TRACE(10, ("__kmp_abt_join_worker: done joining worker T#%d\n",
                th->th.th_info.ds.ds_gtid));

  KMP_MB(); /* Flush all pending memory write invalidates.  */
} // __kmp_abt_join_worker

#endif /* KMP_USE_ABT */

#if KMP_USE_MONITOR
void __kmp_create_monitor(kmp_info_t *th) {
#if !KMP_USE_ABT
  pthread_t handle;
  pthread_attr_t thread_attr;
  size_t size;
  int status;
  int auto_adj_size = FALSE;

  if (__kmp_dflt_blocktime == KMP_MAX_BLOCKTIME) {
    // We don't need monitor thread in case of MAX_BLOCKTIME
    KA_TRACE(10, ("__kmp_create_monitor: skipping monitor thread because of "
                  "MAX blocktime\n"));
    th->th.th_info.ds.ds_tid = 0; // this makes reap_monitor no-op
    th->th.th_info.ds.ds_gtid = 0;
    return;
  }
  KA_TRACE(10, ("__kmp_create_monitor: try to create monitor\n"));

  KMP_MB(); /* Flush all pending memory write invalidates.  */

  th->th.th_info.ds.ds_tid = KMP_GTID_MONITOR;
  th->th.th_info.ds.ds_gtid = KMP_GTID_MONITOR;
#if KMP_REAL_TIME_FIX
  TCW_4(__kmp_global.g.g_time.dt.t_value,
        -1); // Will use it for synchronization a bit later.
#else
  TCW_4(__kmp_global.g.g_time.dt.t_value, 0);
#endif // KMP_REAL_TIME_FIX

#ifdef KMP_THREAD_ATTR
  if (__kmp_monitor_stksize == 0) {
    __kmp_monitor_stksize = KMP_DEFAULT_MONITOR_STKSIZE;
    auto_adj_size = TRUE;
  }
  status = pthread_attr_init(&thread_attr);
  if (status != 0) {
    __kmp_fatal(KMP_MSG(CantInitThreadAttrs), KMP_ERR(status), __kmp_msg_null);
  }
  status = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
  if (status != 0) {
    __kmp_fatal(KMP_MSG(CantSetMonitorState), KMP_ERR(status), __kmp_msg_null);
  }

#ifdef _POSIX_THREAD_ATTR_STACKSIZE
  status = pthread_attr_getstacksize(&thread_attr, &size);
  KMP_CHECK_SYSFAIL("pthread_attr_getstacksize", status);
#else
  size = __kmp_sys_min_stksize;
#endif /* _POSIX_THREAD_ATTR_STACKSIZE */
#endif /* KMP_THREAD_ATTR */

  if (__kmp_monitor_stksize == 0) {
    __kmp_monitor_stksize = KMP_DEFAULT_MONITOR_STKSIZE;
  }
  if (__kmp_monitor_stksize < __kmp_sys_min_stksize) {
    __kmp_monitor_stksize = __kmp_sys_min_stksize;
  }

  KA_TRACE(10, ("__kmp_create_monitor: default stacksize = %lu bytes,"
                "requested stacksize = %lu bytes\n",
                size, __kmp_monitor_stksize));

retry:

/* Set stack size for this thread now. */
#ifdef _POSIX_THREAD_ATTR_STACKSIZE
  KA_TRACE(10, ("__kmp_create_monitor: setting stacksize = %lu bytes,",
                __kmp_monitor_stksize));
  status = pthread_attr_setstacksize(&thread_attr, __kmp_monitor_stksize);
  if (status != 0) {
    if (auto_adj_size) {
      __kmp_monitor_stksize *= 2;
      goto retry;
    }
    kmp_msg_t err_code = KMP_ERR(status);
    __kmp_msg(kmp_ms_warning, // should this be fatal?  BB
              KMP_MSG(CantSetMonitorStackSize, (long int)__kmp_monitor_stksize),
              err_code, KMP_HNT(ChangeMonitorStackSize), __kmp_msg_null);
    if (__kmp_generate_warnings == kmp_warnings_off) {
      __kmp_str_free(&err_code.str);
    }
  }
#endif /* _POSIX_THREAD_ATTR_STACKSIZE */

  status =
      pthread_create(&handle, &thread_attr, __kmp_launch_monitor, (void *)th);

  if (status != 0) {
#ifdef _POSIX_THREAD_ATTR_STACKSIZE
    if (status == EINVAL) {
      if (auto_adj_size && (__kmp_monitor_stksize < (size_t)0x40000000)) {
        __kmp_monitor_stksize *= 2;
        goto retry;
      }
      __kmp_fatal(KMP_MSG(CantSetMonitorStackSize, __kmp_monitor_stksize),
                  KMP_ERR(status), KMP_HNT(IncreaseMonitorStackSize),
                  __kmp_msg_null);
    }
    if (status == ENOMEM) {
      __kmp_fatal(KMP_MSG(CantSetMonitorStackSize, __kmp_monitor_stksize),
                  KMP_ERR(status), KMP_HNT(DecreaseMonitorStackSize),
                  __kmp_msg_null);
    }
#endif /* _POSIX_THREAD_ATTR_STACKSIZE */
    if (status == EAGAIN) {
      __kmp_fatal(KMP_MSG(NoResourcesForMonitorThread), KMP_ERR(status),
                  KMP_HNT(DecreaseNumberOfThreadsInUse), __kmp_msg_null);
    }
    KMP_SYSFAIL("pthread_create", status);
  }

  th->th.th_info.ds.ds_thread = handle;

#if KMP_REAL_TIME_FIX
  // Wait for the monitor thread is really started and set its *priority*.
  KMP_DEBUG_ASSERT(sizeof(kmp_uint32) ==
                   sizeof(__kmp_global.g.g_time.dt.t_value));
  __kmp_wait_yield_4((kmp_uint32 volatile *)&__kmp_global.g.g_time.dt.t_value,
                     -1, &__kmp_neq_4, NULL);
#endif // KMP_REAL_TIME_FIX

#ifdef KMP_THREAD_ATTR
  status = pthread_attr_destroy(&thread_attr);
  if (status != 0) {
    kmp_msg_t err_code = KMP_ERR(status);
    __kmp_msg(kmp_ms_warning, KMP_MSG(CantDestroyThreadAttrs), err_code,
              __kmp_msg_null);
    if (__kmp_generate_warnings == kmp_warnings_off) {
      __kmp_str_free(&err_code.str);
    }
  }
#endif

  KMP_MB(); /* Flush all pending memory write invalidates.  */

  KA_TRACE(10, ("__kmp_create_monitor: monitor created %#.8lx\n",
                th->th.th_info.ds.ds_thread));

#else // !KMP_USE_ABT

  return; // Nothing to do

#endif // KMP_USE_ABT
} // __kmp_create_monitor
#endif // KMP_USE_MONITOR

void __kmp_exit_thread(int exit_status) {
#if KMP_USE_ABT
  ABT_thread_exit();
#else
  pthread_exit((void *)(intptr_t)exit_status);
#endif
} // __kmp_exit_thread

#if KMP_USE_MONITOR
void __kmp_resume_monitor();

void __kmp_reap_monitor(kmp_info_t *th) {
#if !KMP_USE_ABT

  int status;
  void *exit_val;

  KA_TRACE(10, ("__kmp_reap_monitor: try to reap monitor thread with handle"
                " %#.8lx\n",
                th->th.th_info.ds.ds_thread));

  // If monitor has been created, its tid and gtid should be KMP_GTID_MONITOR.
  // If both tid and gtid are 0, it means the monitor did not ever start.
  // If both tid and gtid are KMP_GTID_DNE, the monitor has been shut down.
  KMP_DEBUG_ASSERT(th->th.th_info.ds.ds_tid == th->th.th_info.ds.ds_gtid);
  if (th->th.th_info.ds.ds_gtid != KMP_GTID_MONITOR) {
    KA_TRACE(10, ("__kmp_reap_monitor: monitor did not start, returning\n"));
    return;
  }

  KMP_MB(); /* Flush all pending memory write invalidates.  */

  /* First, check to see whether the monitor thread exists to wake it up. This
     is to avoid performance problem when the monitor sleeps during
     blocktime-size interval */

  status = pthread_kill(th->th.th_info.ds.ds_thread, 0);
  if (status != ESRCH) {
    __kmp_resume_monitor(); // Wake up the monitor thread
  }
  KA_TRACE(10, ("__kmp_reap_monitor: try to join with monitor\n"));
  status = pthread_join(th->th.th_info.ds.ds_thread, &exit_val);
  if (exit_val != th) {
    __kmp_fatal(KMP_MSG(ReapMonitorError), KMP_ERR(status), __kmp_msg_null);
  }

  th->th.th_info.ds.ds_tid = KMP_GTID_DNE;
  th->th.th_info.ds.ds_gtid = KMP_GTID_DNE;

  KA_TRACE(10, ("__kmp_reap_monitor: done reaping monitor thread with handle"
                " %#.8lx\n",
                th->th.th_info.ds.ds_thread));

  KMP_MB(); /* Flush all pending memory write invalidates.  */

#else // !KMP_USE_ABT

  return; // Nothing to do.

#endif // KMP_USE_ABT
}
#endif // KMP_USE_MONITOR

void __kmp_reap_worker(kmp_info_t *th) {
  int status;
  void *exit_val;

  KMP_MB(); /* Flush all pending memory write invalidates.  */

  KA_TRACE(
      10, ("__kmp_reap_worker: try to reap T#%d\n", th->th.th_info.ds.ds_gtid));

#if KMP_USE_ABT

  ABT_thread ds_thread = th->th.th_info.ds.ds_thread;
  if (ds_thread != ABT_THREAD_NULL) {
    status = ABT_thread_free(&ds_thread);
    KMP_ASSERT(status == ABT_SUCCESS);
  }

#else // KMP_USE_ABT

  status = pthread_join(th->th.th_info.ds.ds_thread, &exit_val);
#ifdef KMP_DEBUG
  /* Don't expose these to the user until we understand when they trigger */
  if (status != 0) {
    __kmp_fatal(KMP_MSG(ReapWorkerError), KMP_ERR(status), __kmp_msg_null);
  }
  if (exit_val != th) {
    KA_TRACE(10, ("__kmp_reap_worker: worker T#%d did not reap properly, "
                  "exit_val = %p\n",
                  th->th.th_info.ds.ds_gtid, exit_val));
  }
#endif /* KMP_DEBUG */

  KA_TRACE(10, ("__kmp_reap_worker: done reaping T#%d\n",
                th->th.th_info.ds.ds_gtid));

  KMP_MB(); /* Flush all pending memory write invalidates.  */

#endif // !KMP_USE_ABT
}

#if KMP_HANDLE_SIGNALS

static void __kmp_null_handler(int signo) {
  //  Do nothing, for doing SIG_IGN-type actions.
} // __kmp_null_handler

static void __kmp_team_handler(int signo) {
  if (__kmp_global.g.g_abort == 0) {
/* Stage 1 signal handler, let's shut down all of the threads */
#ifdef KMP_DEBUG
    __kmp_debug_printf("__kmp_team_handler: caught signal = %d\n", signo);
#endif
    switch (signo) {
    case SIGHUP:
    case SIGINT:
    case SIGQUIT:
    case SIGILL:
    case SIGABRT:
    case SIGFPE:
    case SIGBUS:
    case SIGSEGV:
#ifdef SIGSYS
    case SIGSYS:
#endif
    case SIGTERM:
      if (__kmp_debug_buf) {
        __kmp_dump_debug_buffer();
      }
      KMP_MB(); // Flush all pending memory write invalidates.
      TCW_4(__kmp_global.g.g_abort, signo);
      KMP_MB(); // Flush all pending memory write invalidates.
      TCW_4(__kmp_global.g.g_done, TRUE);
      KMP_MB(); // Flush all pending memory write invalidates.
      break;
    default:
#ifdef KMP_DEBUG
      __kmp_debug_printf("__kmp_team_handler: unknown signal type");
#endif
      break;
    }
  }
} // __kmp_team_handler

static void __kmp_sigaction(int signum, const struct sigaction *act,
                            struct sigaction *oldact) {
  int rc = sigaction(signum, act, oldact);
  KMP_CHECK_SYSFAIL_ERRNO("sigaction", rc);
}

static void __kmp_install_one_handler(int sig, sig_func_t handler_func,
                                      int parallel_init) {
  KMP_MB(); // Flush all pending memory write invalidates.
  KB_TRACE(60,
           ("__kmp_install_one_handler( %d, ..., %d )\n", sig, parallel_init));
  if (parallel_init) {
    struct sigaction new_action;
    struct sigaction old_action;
    new_action.sa_handler = handler_func;
    new_action.sa_flags = 0;
    sigfillset(&new_action.sa_mask);
    __kmp_sigaction(sig, &new_action, &old_action);
    if (old_action.sa_handler == __kmp_sighldrs[sig].sa_handler) {
      sigaddset(&__kmp_sigset, sig);
    } else {
      // Restore/keep user's handler if one previously installed.
      __kmp_sigaction(sig, &old_action, NULL);
    }
  } else {
    // Save initial/system signal handlers to see if user handlers installed.
    __kmp_sigaction(sig, NULL, &__kmp_sighldrs[sig]);
  }
  KMP_MB(); // Flush all pending memory write invalidates.
} // __kmp_install_one_handler

static void __kmp_remove_one_handler(int sig) {
  KB_TRACE(60, ("__kmp_remove_one_handler( %d )\n", sig));
  if (sigismember(&__kmp_sigset, sig)) {
    struct sigaction old;
    KMP_MB(); // Flush all pending memory write invalidates.
    __kmp_sigaction(sig, &__kmp_sighldrs[sig], &old);
    if ((old.sa_handler != __kmp_team_handler) &&
        (old.sa_handler != __kmp_null_handler)) {
      // Restore the users signal handler.
      KB_TRACE(10, ("__kmp_remove_one_handler: oops, not our handler, "
                    "restoring: sig=%d\n",
                    sig));
      __kmp_sigaction(sig, &old, NULL);
    }
    sigdelset(&__kmp_sigset, sig);
    KMP_MB(); // Flush all pending memory write invalidates.
  }
} // __kmp_remove_one_handler

void __kmp_install_signals(int parallel_init) {
  KB_TRACE(10, ("__kmp_install_signals( %d )\n", parallel_init));
  if (__kmp_handle_signals || !parallel_init) {
    // If ! parallel_init, we do not install handlers, just save original
    // handlers. Let us do it even __handle_signals is 0.
    sigemptyset(&__kmp_sigset);
    __kmp_install_one_handler(SIGHUP, __kmp_team_handler, parallel_init);
    __kmp_install_one_handler(SIGINT, __kmp_team_handler, parallel_init);
    __kmp_install_one_handler(SIGQUIT, __kmp_team_handler, parallel_init);
    __kmp_install_one_handler(SIGILL, __kmp_team_handler, parallel_init);
    __kmp_install_one_handler(SIGABRT, __kmp_team_handler, parallel_init);
    __kmp_install_one_handler(SIGFPE, __kmp_team_handler, parallel_init);
    __kmp_install_one_handler(SIGBUS, __kmp_team_handler, parallel_init);
    __kmp_install_one_handler(SIGSEGV, __kmp_team_handler, parallel_init);
#ifdef SIGSYS
    __kmp_install_one_handler(SIGSYS, __kmp_team_handler, parallel_init);
#endif // SIGSYS
    __kmp_install_one_handler(SIGTERM, __kmp_team_handler, parallel_init);
#ifdef SIGPIPE
    __kmp_install_one_handler(SIGPIPE, __kmp_team_handler, parallel_init);
#endif // SIGPIPE
  }
} // __kmp_install_signals

void __kmp_remove_signals(void) {
  int sig;
  KB_TRACE(10, ("__kmp_remove_signals()\n"));
  for (sig = 1; sig < NSIG; ++sig) {
    __kmp_remove_one_handler(sig);
  }
} // __kmp_remove_signals

#endif // KMP_HANDLE_SIGNALS

void __kmp_enable(int new_state) {
#ifdef KMP_CANCEL_THREADS
  int status, old_state;
  status = pthread_setcancelstate(new_state, &old_state);
  KMP_CHECK_SYSFAIL("pthread_setcancelstate", status);
  KMP_DEBUG_ASSERT(old_state == PTHREAD_CANCEL_DISABLE);
#endif
}

void __kmp_disable(int *old_state) {
#ifdef KMP_CANCEL_THREADS
  int status;
  status = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, old_state);
  KMP_CHECK_SYSFAIL("pthread_setcancelstate", status);
#endif
}

static void __kmp_atfork_prepare(void) {
  __kmp_acquire_bootstrap_lock(&__kmp_initz_lock);
  __kmp_acquire_bootstrap_lock(&__kmp_forkjoin_lock);
}

static void __kmp_atfork_parent(void) {
  __kmp_release_bootstrap_lock(&__kmp_initz_lock);
  __kmp_release_bootstrap_lock(&__kmp_forkjoin_lock);
}

/* Reset the library so execution in the child starts "all over again" with
   clean data structures in initial states.  Don't worry about freeing memory
   allocated by parent, just abandon it to be safe. */
static void __kmp_atfork_child(void) {
  __kmp_release_bootstrap_lock(&__kmp_forkjoin_lock);
  /* TODO make sure this is done right for nested/sibling */
  // ATT:  Memory leaks are here? TODO: Check it and fix.
  /* KMP_ASSERT( 0 ); */

  ++__kmp_fork_count;

#if KMP_AFFINITY_SUPPORTED
#if KMP_OS_LINUX
  // reset the affinity in the child to the initial thread
  // affinity in the parent
  kmp_set_thread_affinity_mask_initial();
#endif
  // Set default not to bind threads tightly in the child (we’re expecting
  // over-subscription after the fork and this can improve things for
  // scripting languages that use OpenMP inside process-parallel code).
  __kmp_affinity_type = affinity_none;
#if OMP_40_ENABLED
  if (__kmp_nested_proc_bind.bind_types != NULL) {
    __kmp_nested_proc_bind.bind_types[0] = proc_bind_false;
  }
#endif // OMP_40_ENABLED
#endif // KMP_AFFINITY_SUPPORTED

  __kmp_init_runtime = FALSE;
#if KMP_USE_MONITOR
  __kmp_init_monitor = 0;
#endif
  __kmp_init_parallel = FALSE;
  __kmp_init_middle = FALSE;
  __kmp_init_serial = FALSE;
  TCW_4(__kmp_init_gtid, FALSE);
  __kmp_init_common = FALSE;

  TCW_4(__kmp_init_user_locks, FALSE);
#if !KMP_USE_DYNAMIC_LOCK
  __kmp_user_lock_table.used = 1;
  __kmp_user_lock_table.allocated = 0;
  __kmp_user_lock_table.table = NULL;
  __kmp_lock_blocks = NULL;
#endif

  __kmp_all_nth = 0;
  TCW_4(__kmp_nth, 0);

  __kmp_thread_pool = NULL;
  __kmp_thread_pool_insert_pt = NULL;
  __kmp_team_pool = NULL;

  /* Must actually zero all the *cache arguments passed to __kmpc_threadprivate
     here so threadprivate doesn't use stale data */
  KA_TRACE(10, ("__kmp_atfork_child: checking cache address list %p\n",
                __kmp_threadpriv_cache_list));

  while (__kmp_threadpriv_cache_list != NULL) {

    if (*__kmp_threadpriv_cache_list->addr != NULL) {
      KC_TRACE(50, ("__kmp_atfork_child: zeroing cache at address %p\n",
                    &(*__kmp_threadpriv_cache_list->addr)));

      *__kmp_threadpriv_cache_list->addr = NULL;
    }
    __kmp_threadpriv_cache_list = __kmp_threadpriv_cache_list->next;
  }

  __kmp_init_runtime = FALSE;

  /* reset statically initialized locks */
  __kmp_init_bootstrap_lock(&__kmp_initz_lock);
  __kmp_init_bootstrap_lock(&__kmp_stdio_lock);
  __kmp_init_bootstrap_lock(&__kmp_console_lock);
  __kmp_init_bootstrap_lock(&__kmp_task_team_lock);

#if USE_ITT_BUILD
  __kmp_itt_reset(); // reset ITT's global state
#endif /* USE_ITT_BUILD */

  /* This is necessary to make sure no stale data is left around */
  /* AC: customers complain that we use unsafe routines in the atfork
     handler. Mathworks: dlsym() is unsafe. We call dlsym and dlopen
     in dynamic_link when check the presence of shared tbbmalloc library.
     Suggestion is to make the library initialization lazier, similar
     to what done for __kmpc_begin(). */
  // TODO: synchronize all static initializations with regular library
  //       startup; look at kmp_global.cpp and etc.
  //__kmp_internal_begin ();
}

void __kmp_register_atfork(void) {
  if (__kmp_need_register_atfork) {
    int status = pthread_atfork(__kmp_atfork_prepare, __kmp_atfork_parent,
                                __kmp_atfork_child);
    KMP_CHECK_SYSFAIL("pthread_atfork", status);
    __kmp_need_register_atfork = FALSE;
  }
}

void __kmp_suspend_initialize(void) {
#if KMP_USE_ABT
  /* BOLT does not need to initialize them. */
#else
  int status;
  status = pthread_mutexattr_init(&__kmp_suspend_mutex_attr);
  KMP_CHECK_SYSFAIL("pthread_mutexattr_init", status);
  status = pthread_condattr_init(&__kmp_suspend_cond_attr);
  KMP_CHECK_SYSFAIL("pthread_condattr_init", status);
#endif
}

static void __kmp_suspend_initialize_thread(kmp_info_t *th) {
  ANNOTATE_HAPPENS_AFTER(&th->th.th_suspend_init_count);
  if (th->th.th_suspend_init_count <= __kmp_fork_count) {
#if KMP_USE_ABT
  /* BOLT does not need to initialize them. */
#else
    /* this means we haven't initialized the suspension pthread objects for this
       thread in this instance of the process */
    int status;
    status = pthread_cond_init(&th->th.th_suspend_cv.c_cond,
                               &__kmp_suspend_cond_attr);
    KMP_CHECK_SYSFAIL("pthread_cond_init", status);
    status = pthread_mutex_init(&th->th.th_suspend_mx.m_mutex,
                                &__kmp_suspend_mutex_attr);
    KMP_CHECK_SYSFAIL("pthread_mutex_init", status);
#endif
    *(volatile int *)&th->th.th_suspend_init_count = __kmp_fork_count + 1;
    ANNOTATE_HAPPENS_BEFORE(&th->th.th_suspend_init_count);
  }
}

void __kmp_suspend_uninitialize_thread(kmp_info_t *th) {
  if (th->th.th_suspend_init_count > __kmp_fork_count) {
#if KMP_USE_ABT
  /* BOLT does not need to initialize them. */
#else
    /* this means we have initialize the suspension pthread objects for this
       thread in this instance of the process */
    int status;

    status = pthread_cond_destroy(&th->th.th_suspend_cv.c_cond);
    if (status != 0 && status != EBUSY) {
      KMP_SYSFAIL("pthread_cond_destroy", status);
    }
    status = pthread_mutex_destroy(&th->th.th_suspend_mx.m_mutex);
    if (status != 0 && status != EBUSY) {
      KMP_SYSFAIL("pthread_mutex_destroy", status);
    }
#endif
    --th->th.th_suspend_init_count;
    KMP_DEBUG_ASSERT(th->th.th_suspend_init_count == __kmp_fork_count);
  }
}

#if !KMP_USE_ABT
/* This routine puts the calling thread to sleep after setting the
   sleep bit for the indicated flag variable to true. */
template <class C>
static inline void __kmp_suspend_template(int th_gtid, C *flag) {
  KMP_TIME_DEVELOPER_PARTITIONED_BLOCK(USER_suspend);
  kmp_info_t *th = __kmp_threads[th_gtid];
  int status;
  typename C::flag_t old_spin;

  KF_TRACE(30, ("__kmp_suspend_template: T#%d enter for flag = %p\n", th_gtid,
                flag->get()));

  __kmp_suspend_initialize_thread(th);

  status = pthread_mutex_lock(&th->th.th_suspend_mx.m_mutex);
  KMP_CHECK_SYSFAIL("pthread_mutex_lock", status);

  KF_TRACE(10, ("__kmp_suspend_template: T#%d setting sleep bit for spin(%p)\n",
                th_gtid, flag->get()));

  /* TODO: shouldn't this use release semantics to ensure that
     __kmp_suspend_initialize_thread gets called first? */
  old_spin = flag->set_sleeping();

  KF_TRACE(5, ("__kmp_suspend_template: T#%d set sleep bit for spin(%p)==%x,"
               " was %x\n",
               th_gtid, flag->get(), flag->load(), old_spin));

  if (flag->done_check_val(old_spin)) {
    old_spin = flag->unset_sleeping();
    KF_TRACE(5, ("__kmp_suspend_template: T#%d false alarm, reset sleep bit "
                 "for spin(%p)\n",
                 th_gtid, flag->get()));
  } else {
    /* Encapsulate in a loop as the documentation states that this may
       "with low probability" return when the condition variable has
       not been signaled or broadcast */
    int deactivated = FALSE;
    TCW_PTR(th->th.th_sleep_loc, (void *)flag);

    while (flag->is_sleeping()) {
#ifdef DEBUG_SUSPEND
      char buffer[128];
      __kmp_suspend_count++;
      __kmp_print_cond(buffer, &th->th.th_suspend_cv);
      __kmp_printf("__kmp_suspend_template: suspending T#%d: %s\n", th_gtid,
                   buffer);
#endif
      // Mark the thread as no longer active (only in the first iteration of the
      // loop).
      if (!deactivated) {
        th->th.th_active = FALSE;
        if (th->th.th_active_in_pool) {
          th->th.th_active_in_pool = FALSE;
          KMP_ATOMIC_DEC(&__kmp_thread_pool_active_nth);
          KMP_DEBUG_ASSERT(TCR_4(__kmp_thread_pool_active_nth) >= 0);
        }
        deactivated = TRUE;
      }

#if USE_SUSPEND_TIMEOUT
      struct timespec now;
      struct timeval tval;
      int msecs;

      status = gettimeofday(&tval, NULL);
      KMP_CHECK_SYSFAIL_ERRNO("gettimeofday", status);
      TIMEVAL_TO_TIMESPEC(&tval, &now);

      msecs = (4 * __kmp_dflt_blocktime) + 200;
      now.tv_sec += msecs / 1000;
      now.tv_nsec += (msecs % 1000) * 1000;

      KF_TRACE(15, ("__kmp_suspend_template: T#%d about to perform "
                    "pthread_cond_timedwait\n",
                    th_gtid));
      status = pthread_cond_timedwait(&th->th.th_suspend_cv.c_cond,
                                      &th->th.th_suspend_mx.m_mutex, &now);
#else
      KF_TRACE(15, ("__kmp_suspend_template: T#%d about to perform"
                    " pthread_cond_wait\n",
                    th_gtid));
      status = pthread_cond_wait(&th->th.th_suspend_cv.c_cond,
                                 &th->th.th_suspend_mx.m_mutex);
#endif

      if ((status != 0) && (status != EINTR) && (status != ETIMEDOUT)) {
        KMP_SYSFAIL("pthread_cond_wait", status);
      }
#ifdef KMP_DEBUG
      if (status == ETIMEDOUT) {
        if (flag->is_sleeping()) {
          KF_TRACE(100,
                   ("__kmp_suspend_template: T#%d timeout wakeup\n", th_gtid));
        } else {
          KF_TRACE(2, ("__kmp_suspend_template: T#%d timeout wakeup, sleep bit "
                       "not set!\n",
                       th_gtid));
        }
      } else if (flag->is_sleeping()) {
        KF_TRACE(100,
                 ("__kmp_suspend_template: T#%d spurious wakeup\n", th_gtid));
      }
#endif
    } // while

    // Mark the thread as active again (if it was previous marked as inactive)
    if (deactivated) {
      th->th.th_active = TRUE;
      if (TCR_4(th->th.th_in_pool)) {
        KMP_ATOMIC_INC(&__kmp_thread_pool_active_nth);
        th->th.th_active_in_pool = TRUE;
      }
    }
  }
#ifdef DEBUG_SUSPEND
  {
    char buffer[128];
    __kmp_print_cond(buffer, &th->th.th_suspend_cv);
    __kmp_printf("__kmp_suspend_template: T#%d has awakened: %s\n", th_gtid,
                 buffer);
  }
#endif

  status = pthread_mutex_unlock(&th->th.th_suspend_mx.m_mutex);
  KMP_CHECK_SYSFAIL("pthread_mutex_unlock", status);
  KF_TRACE(30, ("__kmp_suspend_template: T#%d exit\n", th_gtid));
}

void __kmp_suspend_32(int th_gtid, kmp_flag_32 *flag) {
  __kmp_suspend_template(th_gtid, flag);
}
void __kmp_suspend_64(int th_gtid, kmp_flag_64 *flag) {
  __kmp_suspend_template(th_gtid, flag);
}
void __kmp_suspend_oncore(int th_gtid, kmp_flag_oncore *flag) {
  __kmp_suspend_template(th_gtid, flag);
}

/* This routine signals the thread specified by target_gtid to wake up
   after setting the sleep bit indicated by the flag argument to FALSE.
   The target thread must already have called __kmp_suspend_template() */
template <class C>
static inline void __kmp_resume_template(int target_gtid, C *flag) {
  KMP_TIME_DEVELOPER_PARTITIONED_BLOCK(USER_resume);
  kmp_info_t *th = __kmp_threads[target_gtid];
  int status;

#ifdef KMP_DEBUG
  int gtid = TCR_4(__kmp_init_gtid) ? __kmp_get_gtid() : -1;
#endif

  KF_TRACE(30, ("__kmp_resume_template: T#%d wants to wakeup T#%d enter\n",
                gtid, target_gtid));
  KMP_DEBUG_ASSERT(gtid != target_gtid);

  __kmp_suspend_initialize_thread(th);

  status = pthread_mutex_lock(&th->th.th_suspend_mx.m_mutex);
  KMP_CHECK_SYSFAIL("pthread_mutex_lock", status);

  if (!flag) { // coming from __kmp_null_resume_wrapper
    flag = (C *)CCAST(void *, th->th.th_sleep_loc);
  }

  // First, check if the flag is null or its type has changed. If so, someone
  // else woke it up.
  if (!flag || flag->get_type() != flag->get_ptr_type()) { // get_ptr_type
    // simply shows what
    // flag was cast to
    KF_TRACE(5, ("__kmp_resume_template: T#%d exiting, thread T#%d already "
                 "awake: flag(%p)\n",
                 gtid, target_gtid, NULL));
    status = pthread_mutex_unlock(&th->th.th_suspend_mx.m_mutex);
    KMP_CHECK_SYSFAIL("pthread_mutex_unlock", status);
    return;
  } else { // if multiple threads are sleeping, flag should be internally
    // referring to a specific thread here
    typename C::flag_t old_spin = flag->unset_sleeping();
    if (!flag->is_sleeping_val(old_spin)) {
      KF_TRACE(5, ("__kmp_resume_template: T#%d exiting, thread T#%d already "
                   "awake: flag(%p): "
                   "%u => %u\n",
                   gtid, target_gtid, flag->get(), old_spin, flag->load()));
      status = pthread_mutex_unlock(&th->th.th_suspend_mx.m_mutex);
      KMP_CHECK_SYSFAIL("pthread_mutex_unlock", status);
      return;
    }
    KF_TRACE(5, ("__kmp_resume_template: T#%d about to wakeup T#%d, reset "
                 "sleep bit for flag's loc(%p): "
                 "%u => %u\n",
                 gtid, target_gtid, flag->get(), old_spin, flag->load()));
  }
  TCW_PTR(th->th.th_sleep_loc, NULL);

#ifdef DEBUG_SUSPEND
  {
    char buffer[128];
    __kmp_print_cond(buffer, &th->th.th_suspend_cv);
    __kmp_printf("__kmp_resume_template: T#%d resuming T#%d: %s\n", gtid,
                 target_gtid, buffer);
  }
#endif
  status = pthread_cond_signal(&th->th.th_suspend_cv.c_cond);
  KMP_CHECK_SYSFAIL("pthread_cond_signal", status);
  status = pthread_mutex_unlock(&th->th.th_suspend_mx.m_mutex);
  KMP_CHECK_SYSFAIL("pthread_mutex_unlock", status);
  KF_TRACE(30, ("__kmp_resume_template: T#%d exiting after signaling wake up"
                " for T#%d\n",
                gtid, target_gtid));
}

void __kmp_resume_32(int target_gtid, kmp_flag_32 *flag) {
  __kmp_resume_template(target_gtid, flag);
}
void __kmp_resume_64(int target_gtid, kmp_flag_64 *flag) {
  __kmp_resume_template(target_gtid, flag);
}
void __kmp_resume_oncore(int target_gtid, kmp_flag_oncore *flag) {
  __kmp_resume_template(target_gtid, flag);
}

#if KMP_USE_MONITOR
void __kmp_resume_monitor() {
  KMP_TIME_DEVELOPER_PARTITIONED_BLOCK(USER_resume);
  int status;
#ifdef KMP_DEBUG
  int gtid = TCR_4(__kmp_init_gtid) ? __kmp_get_gtid() : -1;
  KF_TRACE(30, ("__kmp_resume_monitor: T#%d wants to wakeup T#%d enter\n", gtid,
                KMP_GTID_MONITOR));
  KMP_DEBUG_ASSERT(gtid != KMP_GTID_MONITOR);
#endif
  status = pthread_mutex_lock(&__kmp_wait_mx.m_mutex);
  KMP_CHECK_SYSFAIL("pthread_mutex_lock", status);
#ifdef DEBUG_SUSPEND
  {
    char buffer[128];
    __kmp_print_cond(buffer, &__kmp_wait_cv.c_cond);
    __kmp_printf("__kmp_resume_monitor: T#%d resuming T#%d: %s\n", gtid,
                 KMP_GTID_MONITOR, buffer);
  }
#endif
  status = pthread_cond_signal(&__kmp_wait_cv.c_cond);
  KMP_CHECK_SYSFAIL("pthread_cond_signal", status);
  status = pthread_mutex_unlock(&__kmp_wait_mx.m_mutex);
  KMP_CHECK_SYSFAIL("pthread_mutex_unlock", status);
  KF_TRACE(30, ("__kmp_resume_monitor: T#%d exiting after signaling wake up"
                " for T#%d\n",
                gtid, KMP_GTID_MONITOR));
}
#endif // KMP_USE_MONITOR

#endif // !KMP_USE_ABT

void __kmp_yield(int cond) {
  if (!cond)
    return;
#if KMP_USE_MONITOR
  if (!__kmp_yielding_on)
    return;
#else
  if (__kmp_yield_cycle && !KMP_YIELD_NOW())
    return;
#endif
#if KMP_USE_ABT
  ABT_thread_yield();
#else
  sched_yield();
#endif
}

void __kmp_gtid_set_specific(int gtid) {
#if KMP_USE_ABT
  ABT_thread self;
  kmp_info_t *th;
  KMP_ASSERT(__kmp_init_runtime);
  ABT_thread_self(&self);

  if (self != ABT_THREAD_NULL) {
    ABT_thread_get_arg(self, (void **)&th);
    KMP_ASSERT(th != NULL);
    th->th.th_info.ds.ds_gtid = gtid;
    KMP_ASSERT(__kmp_init_gtid);
    return;
  }
#endif // KMP_USE_ABT
  if (__kmp_init_gtid) {
    int status;
    status = pthread_setspecific(__kmp_gtid_threadprivate_key,
                                 (void *)(intptr_t)(gtid + 1));
    KMP_CHECK_SYSFAIL("pthread_setspecific", status);
  } else {
    KA_TRACE(50, ("__kmp_gtid_set_specific: runtime shutdown, returning\n"));
  }
}

int __kmp_gtid_get_specific() {
  int gtid;
#if KMP_USE_ABT

  ABT_thread self;
  ABT_thread_self(&self);
  if (self == ABT_THREAD_NULL) {
    KMP_ASSERT(__kmp_init_gtid);
    /* External threads might call OpenMP functions. */
    gtid = (int)(size_t)pthread_getspecific(__kmp_gtid_threadprivate_key);
    KA_TRACE(50, ("__kmp_gtid_get_specific: key:%d gtid:%d\n",
                  __kmp_gtid_threadprivate_key, gtid));
  } else {
    kmp_info_t *th;
    ABT_thread_get_arg(self, (void **)&th);
    if (th == NULL) {
      gtid = KMP_GTID_DNE;
    } else {
      gtid = th->th.th_info.ds.ds_gtid;
    }
    KA_TRACE(50, ("__kmp_gtid_get_specific: ULT:%p gtid:%d\n", self, gtid));
  }

#else // KMP_USE_ABT

  if (!__kmp_init_gtid) {
    KA_TRACE(50, ("__kmp_gtid_get_specific: runtime shutdown, returning "
                  "KMP_GTID_SHUTDOWN\n"));
    return KMP_GTID_SHUTDOWN;
  }
  gtid = (int)(size_t)pthread_getspecific(__kmp_gtid_threadprivate_key);
  if (gtid == 0) {
    gtid = KMP_GTID_DNE;
  } else {
    gtid--;
  }
  KA_TRACE(50, ("__kmp_gtid_get_specific: key:%d gtid:%d\n",
                __kmp_gtid_threadprivate_key, gtid));

#endif // !KMP_USE_ABT
  return gtid;
}

double __kmp_read_cpu_time(void) {
  /*clock_t   t;*/
  struct tms buffer;

  /*t =*/times(&buffer);

  return (buffer.tms_utime + buffer.tms_cutime) / (double)CLOCKS_PER_SEC;
}

int __kmp_read_system_info(struct kmp_sys_info *info) {
  int status;
  struct rusage r_usage;

  memset(info, 0, sizeof(*info));

  status = getrusage(RUSAGE_SELF, &r_usage);
  KMP_CHECK_SYSFAIL_ERRNO("getrusage", status);

  // The maximum resident set size utilized (in kilobytes)
  info->maxrss = r_usage.ru_maxrss;
  // The number of page faults serviced without any I/O
  info->minflt = r_usage.ru_minflt;
  // The number of page faults serviced that required I/O
  info->majflt = r_usage.ru_majflt;
  // The number of times a process was "swapped" out of memory
  info->nswap = r_usage.ru_nswap;
  // The number of times the file system had to perform input
  info->inblock = r_usage.ru_inblock;
  // The number of times the file system had to perform output
  info->oublock = r_usage.ru_oublock;
  // The number of times a context switch was voluntarily
  info->nvcsw = r_usage.ru_nvcsw;
  // The number of times a context switch was forced
  info->nivcsw = r_usage.ru_nivcsw;

  return (status != 0);
}

void __kmp_read_system_time(double *delta) {
  double t_ns;
  struct timeval tval;
  struct timespec stop;
  int status;

  status = gettimeofday(&tval, NULL);
  KMP_CHECK_SYSFAIL_ERRNO("gettimeofday", status);
  TIMEVAL_TO_TIMESPEC(&tval, &stop);
  t_ns = TS2NS(stop) - TS2NS(__kmp_sys_timer_data.start);
  *delta = (t_ns * 1e-9);
}

void __kmp_clear_system_time(void) {
  struct timeval tval;
  int status;
  status = gettimeofday(&tval, NULL);
  KMP_CHECK_SYSFAIL_ERRNO("gettimeofday", status);
  TIMEVAL_TO_TIMESPEC(&tval, &__kmp_sys_timer_data.start);
}

static int __kmp_get_xproc(void) {

  int r = 0;

#if KMP_OS_LINUX || KMP_OS_FREEBSD || KMP_OS_NETBSD

  r = sysconf(_SC_NPROCESSORS_ONLN);

#elif KMP_OS_DARWIN

  // Bug C77011 High "OpenMP Threads and number of active cores".

  // Find the number of available CPUs.
  kern_return_t rc;
  host_basic_info_data_t info;
  mach_msg_type_number_t num = HOST_BASIC_INFO_COUNT;
  rc = host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t)&info, &num);
  if (rc == 0 && num == HOST_BASIC_INFO_COUNT) {
    // Cannot use KA_TRACE() here because this code works before trace support
    // is initialized.
    r = info.avail_cpus;
  } else {
    KMP_WARNING(CantGetNumAvailCPU);
    KMP_INFORM(AssumedNumCPU);
  }

#else

#error "Unknown or unsupported OS."

#endif

  return r > 0 ? r : 2; /* guess value of 2 if OS told us 0 */

} // __kmp_get_xproc

int __kmp_read_from_file(char const *path, char const *format, ...) {
  int result;
  va_list args;

  va_start(args, format);
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return 0;
  result = vfscanf(f, format, args);
  fclose(f);

  return result;
}

void __kmp_runtime_initialize(void) {
  int status;
#if !KMP_USE_ABT
  pthread_mutexattr_t mutex_attr;
  pthread_condattr_t cond_attr;
#endif

  if (__kmp_init_runtime) {
    return;
  }

#if (KMP_ARCH_X86 || KMP_ARCH_X86_64)
  if (!__kmp_cpuinfo.initialized) {
    __kmp_query_cpuid(&__kmp_cpuinfo);
  }
#endif /* KMP_ARCH_X86 || KMP_ARCH_X86_64 */

  __kmp_xproc = __kmp_get_xproc();

  if (sysconf(_SC_THREADS)) {

    /* Query the maximum number of threads */
    __kmp_sys_max_nth = sysconf(_SC_THREAD_THREADS_MAX);
    if (__kmp_sys_max_nth == -1) {
      /* Unlimited threads for NPTL */
      __kmp_sys_max_nth = INT_MAX;
    } else if (__kmp_sys_max_nth <= 1) {
      /* Can't tell, just use PTHREAD_THREADS_MAX */
      __kmp_sys_max_nth = KMP_MAX_NTH;
    }

    /* Query the minimum stack size */
    __kmp_sys_min_stksize = sysconf(_SC_THREAD_STACK_MIN);
    if (__kmp_sys_min_stksize <= 1) {
      __kmp_sys_min_stksize = KMP_MIN_STKSIZE;
    }
  }

  /* Set up minimum number of threads to switch to TLS gtid */
  __kmp_tls_gtid_min = KMP_TLS_GTID_MIN;

  status = pthread_key_create(&__kmp_gtid_threadprivate_key,
                              __kmp_internal_end_dest);
  KMP_CHECK_SYSFAIL("pthread_key_create", status);
#if KMP_USE_ABT
  __kmp_abt_initialize();
#else
  status = pthread_mutexattr_init(&mutex_attr);
  KMP_CHECK_SYSFAIL("pthread_mutexattr_init", status);
  status = pthread_mutex_init(&__kmp_wait_mx.m_mutex, &mutex_attr);
  KMP_CHECK_SYSFAIL("pthread_mutex_init", status);
  status = pthread_condattr_init(&cond_attr);
  KMP_CHECK_SYSFAIL("pthread_condattr_init", status);
  status = pthread_cond_init(&__kmp_wait_cv.c_cond, &cond_attr);
  KMP_CHECK_SYSFAIL("pthread_cond_init", status);
#endif
#if USE_ITT_BUILD
  __kmp_itt_initialize();
#endif /* USE_ITT_BUILD */

  __kmp_init_runtime = TRUE;
}

void __kmp_runtime_destroy(void) {
  int status;

  if (!__kmp_init_runtime) {
    return; // Nothing to do.
  }

#if USE_ITT_BUILD
  __kmp_itt_destroy();
#endif /* USE_ITT_BUILD */

  status = pthread_key_delete(__kmp_gtid_threadprivate_key);
  KMP_CHECK_SYSFAIL("pthread_key_delete", status);

#if KMP_USE_ABT
  __kmp_abt_finalize();
#else
  status = pthread_mutex_destroy(&__kmp_wait_mx.m_mutex);
  if (status != 0 && status != EBUSY) {
    KMP_SYSFAIL("pthread_mutex_destroy", status);
  }
  status = pthread_cond_destroy(&__kmp_wait_cv.c_cond);
  if (status != 0 && status != EBUSY) {
    KMP_SYSFAIL("pthread_cond_destroy", status);
  }
#endif
#if KMP_AFFINITY_SUPPORTED
  __kmp_affinity_uninitialize();
#endif

  __kmp_init_runtime = FALSE;
}

/* Put the thread to sleep for a time period */
/* NOTE: not currently used anywhere */
void __kmp_thread_sleep(int millis) { sleep((millis + 500) / 1000); }

/* Calculate the elapsed wall clock time for the user */
void __kmp_elapsed(double *t) {
  int status;
#ifdef FIX_SGI_CLOCK
  struct timespec ts;

  status = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  KMP_CHECK_SYSFAIL_ERRNO("clock_gettime", status);
  *t =
      (double)ts.tv_nsec * (1.0 / (double)KMP_NSEC_PER_SEC) + (double)ts.tv_sec;
#else
  struct timeval tv;

  status = gettimeofday(&tv, NULL);
  KMP_CHECK_SYSFAIL_ERRNO("gettimeofday", status);
  *t =
      (double)tv.tv_usec * (1.0 / (double)KMP_USEC_PER_SEC) + (double)tv.tv_sec;
#endif
}

/* Calculate the elapsed wall clock tick for the user */
void __kmp_elapsed_tick(double *t) { *t = 1 / (double)CLOCKS_PER_SEC; }

/* Return the current time stamp in nsec */
kmp_uint64 __kmp_now_nsec() {
  struct timeval t;
  gettimeofday(&t, NULL);
  return KMP_NSEC_PER_SEC * t.tv_sec + 1000 * t.tv_usec;
}

#if KMP_ARCH_X86 || KMP_ARCH_X86_64
/* Measure clock ticks per millisecond */
void __kmp_initialize_system_tick() {
  kmp_uint64 delay = 100000; // 50~100 usec on most machines.
  kmp_uint64 nsec = __kmp_now_nsec();
  kmp_uint64 goal = __kmp_hardware_timestamp() + delay;
  kmp_uint64 now;
  while ((now = __kmp_hardware_timestamp()) < goal)
    ;
  __kmp_ticks_per_msec =
      (kmp_uint64)(1e6 * (delay + (now - goal)) / (__kmp_now_nsec() - nsec));
}
#endif

/* Determine whether the given address is mapped into the current address
   space. */

int __kmp_is_address_mapped(void *addr) {

  int found = 0;
  int rc;

#if KMP_OS_LINUX || KMP_OS_FREEBSD

  /* On Linux* OS, read the /proc/<pid>/maps pseudo-file to get all the address
     ranges mapped into the address space. */

  char *name = __kmp_str_format("/proc/%d/maps", getpid());
  FILE *file = NULL;

  file = fopen(name, "r");
  KMP_ASSERT(file != NULL);

  for (;;) {

    void *beginning = NULL;
    void *ending = NULL;
    char perms[5];

    rc = fscanf(file, "%p-%p %4s %*[^\n]\n", &beginning, &ending, perms);
    if (rc == EOF) {
      break;
    }
    KMP_ASSERT(rc == 3 &&
               KMP_STRLEN(perms) == 4); // Make sure all fields are read.

    // Ending address is not included in the region, but beginning is.
    if ((addr >= beginning) && (addr < ending)) {
      perms[2] = 0; // 3th and 4th character does not matter.
      if (strcmp(perms, "rw") == 0) {
        // Memory we are looking for should be readable and writable.
        found = 1;
      }
      break;
    }
  }

  // Free resources.
  fclose(file);
  KMP_INTERNAL_FREE(name);

#elif KMP_OS_DARWIN

  /* On OS X*, /proc pseudo filesystem is not available. Try to read memory
     using vm interface. */

  int buffer;
  vm_size_t count;
  rc = vm_read_overwrite(
      mach_task_self(), // Task to read memory of.
      (vm_address_t)(addr), // Address to read from.
      1, // Number of bytes to be read.
      (vm_address_t)(&buffer), // Address of buffer to save read bytes in.
      &count // Address of var to save number of read bytes in.
      );
  if (rc == 0) {
    // Memory successfully read.
    found = 1;
  }

#elif KMP_OS_FREEBSD || KMP_OS_NETBSD

  // FIXME(FreeBSD, NetBSD): Implement this
  found = 1;

#else

#error "Unknown or unsupported OS"

#endif

  return found;

} // __kmp_is_address_mapped

#ifdef USE_LOAD_BALANCE

#if KMP_OS_DARWIN

// The function returns the rounded value of the system load average
// during given time interval which depends on the value of
// __kmp_load_balance_interval variable (default is 60 sec, other values
// may be 300 sec or 900 sec).
// It returns -1 in case of error.
int __kmp_get_load_balance(int max) {
  double averages[3];
  int ret_avg = 0;

  int res = getloadavg(averages, 3);

  // Check __kmp_load_balance_interval to determine which of averages to use.
  // getloadavg() may return the number of samples less than requested that is
  // less than 3.
  if (__kmp_load_balance_interval < 180 && (res >= 1)) {
    ret_avg = averages[0]; // 1 min
  } else if ((__kmp_load_balance_interval >= 180 &&
              __kmp_load_balance_interval < 600) &&
             (res >= 2)) {
    ret_avg = averages[1]; // 5 min
  } else if ((__kmp_load_balance_interval >= 600) && (res == 3)) {
    ret_avg = averages[2]; // 15 min
  } else { // Error occurred
    return -1;
  }

  return ret_avg;
}

#else // Linux* OS

// The fuction returns number of running (not sleeping) threads, or -1 in case
// of error. Error could be reported if Linux* OS kernel too old (without
// "/proc" support). Counting running threads stops if max running threads
// encountered.
int __kmp_get_load_balance(int max) {
  static int permanent_error = 0;
  static int glb_running_threads = 0; // Saved count of the running threads for
  // the thread balance algortihm
  static double glb_call_time = 0; /* Thread balance algorithm call time */

  int running_threads = 0; // Number of running threads in the system.

  DIR *proc_dir = NULL; // Handle of "/proc/" directory.
  struct dirent *proc_entry = NULL;

  kmp_str_buf_t task_path; // "/proc/<pid>/task/<tid>/" path.
  DIR *task_dir = NULL; // Handle of "/proc/<pid>/task/<tid>/" directory.
  struct dirent *task_entry = NULL;
  int task_path_fixed_len;

  kmp_str_buf_t stat_path; // "/proc/<pid>/task/<tid>/stat" path.
  int stat_file = -1;
  int stat_path_fixed_len;

  int total_processes = 0; // Total number of processes in system.
  int total_threads = 0; // Total number of threads in system.

  double call_time = 0.0;

  __kmp_str_buf_init(&task_path);
  __kmp_str_buf_init(&stat_path);

  __kmp_elapsed(&call_time);

  if (glb_call_time &&
      (call_time - glb_call_time < __kmp_load_balance_interval)) {
    running_threads = glb_running_threads;
    goto finish;
  }

  glb_call_time = call_time;

  // Do not spend time on scanning "/proc/" if we have a permanent error.
  if (permanent_error) {
    running_threads = -1;
    goto finish;
  }

  if (max <= 0) {
    max = INT_MAX;
  }

  // Open "/proc/" directory.
  proc_dir = opendir("/proc");
  if (proc_dir == NULL) {
    // Cannot open "/prroc/". Probably the kernel does not support it. Return an
    // error now and in subsequent calls.
    running_threads = -1;
    permanent_error = 1;
    goto finish;
  }

  // Initialize fixed part of task_path. This part will not change.
  __kmp_str_buf_cat(&task_path, "/proc/", 6);
  task_path_fixed_len = task_path.used; // Remember number of used characters.

  proc_entry = readdir(proc_dir);
  while (proc_entry != NULL) {
    // Proc entry is a directory and name starts with a digit. Assume it is a
    // process' directory.
    if (proc_entry->d_type == DT_DIR && isdigit(proc_entry->d_name[0])) {

      ++total_processes;
      // Make sure init process is the very first in "/proc", so we can replace
      // strcmp( proc_entry->d_name, "1" ) == 0 with simpler total_processes ==
      // 1. We are going to check that total_processes == 1 => d_name == "1" is
      // true (where "=>" is implication). Since C++ does not have => operator,
      // let us replace it with its equivalent: a => b == ! a || b.
      KMP_DEBUG_ASSERT(total_processes != 1 ||
                       strcmp(proc_entry->d_name, "1") == 0);

      // Construct task_path.
      task_path.used = task_path_fixed_len; // Reset task_path to "/proc/".
      __kmp_str_buf_cat(&task_path, proc_entry->d_name,
                        KMP_STRLEN(proc_entry->d_name));
      __kmp_str_buf_cat(&task_path, "/task", 5);

      task_dir = opendir(task_path.str);
      if (task_dir == NULL) {
        // Process can finish between reading "/proc/" directory entry and
        // opening process' "task/" directory. So, in general case we should not
        // complain, but have to skip this process and read the next one. But on
        // systems with no "task/" support we will spend lot of time to scan
        // "/proc/" tree again and again without any benefit. "init" process
        // (its pid is 1) should exist always, so, if we cannot open
        // "/proc/1/task/" directory, it means "task/" is not supported by
        // kernel. Report an error now and in the future.
        if (strcmp(proc_entry->d_name, "1") == 0) {
          running_threads = -1;
          permanent_error = 1;
          goto finish;
        }
      } else {
        // Construct fixed part of stat file path.
        __kmp_str_buf_clear(&stat_path);
        __kmp_str_buf_cat(&stat_path, task_path.str, task_path.used);
        __kmp_str_buf_cat(&stat_path, "/", 1);
        stat_path_fixed_len = stat_path.used;

        task_entry = readdir(task_dir);
        while (task_entry != NULL) {
          // It is a directory and name starts with a digit.
          if (proc_entry->d_type == DT_DIR && isdigit(task_entry->d_name[0])) {
            ++total_threads;

            // Consruct complete stat file path. Easiest way would be:
            //  __kmp_str_buf_print( & stat_path, "%s/%s/stat", task_path.str,
            //  task_entry->d_name );
            // but seriae of __kmp_str_buf_cat works a bit faster.
            stat_path.used =
                stat_path_fixed_len; // Reset stat path to its fixed part.
            __kmp_str_buf_cat(&stat_path, task_entry->d_name,
                              KMP_STRLEN(task_entry->d_name));
            __kmp_str_buf_cat(&stat_path, "/stat", 5);

            // Note: Low-level API (open/read/close) is used. High-level API
            // (fopen/fclose)  works ~ 30 % slower.
            stat_file = open(stat_path.str, O_RDONLY);
            if (stat_file == -1) {
              // We cannot report an error because task (thread) can terminate
              // just before reading this file.
            } else {
              /* Content of "stat" file looks like:
                 24285 (program) S ...

                 It is a single line (if program name does not include funny
                 symbols). First number is a thread id, then name of executable
                 file name in paretheses, then state of the thread. We need just
                 thread state.

                 Good news: Length of program name is 15 characters max. Longer
                 names are truncated.

                 Thus, we need rather short buffer: 15 chars for program name +
                 2 parenthesis, + 3 spaces + ~7 digits of pid = 37.

                 Bad news: Program name may contain special symbols like space,
                 closing parenthesis, or even new line. This makes parsing
                 "stat" file not 100 % reliable. In case of fanny program names
                 parsing may fail (report incorrect thread state).

                 Parsing "status" file looks more promissing (due to different
                 file structure and escaping special symbols) but reading and
                 parsing of "status" file works slower.
                  -- ln
              */
              char buffer[65];
              int len;
              len = read(stat_file, buffer, sizeof(buffer) - 1);
              if (len >= 0) {
                buffer[len] = 0;
                // Using scanf:
                //     sscanf( buffer, "%*d (%*s) %c ", & state );
                // looks very nice, but searching for a closing parenthesis
                // works a bit faster.
                char *close_parent = strstr(buffer, ") ");
                if (close_parent != NULL) {
                  char state = *(close_parent + 2);
                  if (state == 'R') {
                    ++running_threads;
                    if (running_threads >= max) {
                      goto finish;
                    }
                  }
                }
              }
              close(stat_file);
              stat_file = -1;
            }
          }
          task_entry = readdir(task_dir);
        }
        closedir(task_dir);
        task_dir = NULL;
      }
    }
    proc_entry = readdir(proc_dir);
  }

  // There _might_ be a timing hole where the thread executing this
  // code get skipped in the load balance, and running_threads is 0.
  // Assert in the debug builds only!!!
  KMP_DEBUG_ASSERT(running_threads > 0);
  if (running_threads <= 0) {
    running_threads = 1;
  }

finish: // Clean up and exit.
  if (proc_dir != NULL) {
    closedir(proc_dir);
  }
  __kmp_str_buf_free(&task_path);
  if (task_dir != NULL) {
    closedir(task_dir);
  }
  __kmp_str_buf_free(&stat_path);
  if (stat_file != -1) {
    close(stat_file);
  }

  glb_running_threads = running_threads;

  return running_threads;

} // __kmp_get_load_balance

#endif // KMP_OS_DARWIN

#endif // USE_LOAD_BALANCE

#if KMP_USE_ABT || !(KMP_ARCH_X86 || KMP_ARCH_X86_64 || KMP_MIC ||             \
      ((KMP_OS_LINUX || KMP_OS_DARWIN) && KMP_ARCH_AARCH64) || KMP_ARCH_PPC64)

// we really only need the case with 1 argument, because CLANG always build
// a struct of pointers to shared variables referenced in the outlined function
int __kmp_invoke_microtask(microtask_t pkfn, int gtid, int tid, int argc,
                           void *p_argv[]
#if OMPT_SUPPORT
                           ,
                           void **exit_frame_ptr
#endif
                           ) {
#if OMPT_SUPPORT
  *exit_frame_ptr = OMPT_GET_FRAME_ADDRESS(0);
#endif

  switch (argc) {
  default:
    fprintf(stderr, "Too many args to microtask: %d!\n", argc);
    fflush(stderr);
    exit(-1);
  case 0:
    (*pkfn)(&gtid, &tid);
    break;
  case 1:
    (*pkfn)(&gtid, &tid, p_argv[0]);
    break;
  case 2:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1]);
    break;
  case 3:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2]);
    break;
  case 4:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3]);
    break;
  case 5:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4]);
    break;
  case 6:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5]);
    break;
  case 7:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6]);
    break;
  case 8:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7]);
    break;
  case 9:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8]);
    break;
  case 10:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9]);
    break;
  case 11:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10]);
    break;
  case 12:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11]);
    break;
  case 13:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12]);
    break;
  case 14:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13]);
    break;
  case 15:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14]);
    break;
  case 16:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15]);
    break;
  case 17:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16]);
    break;
  case 18:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17]);
    break;
  case 19:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18]);
    break;
  case 20:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19]);
    break;
  case 21:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20]);
    break;
  case 22:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21]);
    break;
  case 23:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22]);
    break;
  case 24:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23]);
    break;
  case 25:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24]);
    break;
  case 26:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25]);
    break;
  case 27:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26]);
    break;
  case 28:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27]);
    break;
  case 29:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28]);
    break;
  case 30:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29]);
    break;
  case 31:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30]);
    break;
  case 32:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31]);
    break;
  case 33:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32]);
    break;
  case 34:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33]);
    break;
  case 35:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34]);
    break;
  case 36:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35]);
    break;
  case 37:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36]);
    break;
  case 38:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37]);
    break;
  case 39:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38]);
    break;
  case 40:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39]);
    break;
  case 41:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40]);
    break;
  case 42:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41]);
    break;
  case 43:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42]);
    break;
  case 44:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43]);
    break;
  case 45:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44]);
    break;
  case 46:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45]);
    break;
  case 47:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46]);
    break;
  case 48:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47]);
    break;
  case 49:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48]);
    break;
  case 50:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49]);
    break;
  case 51:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50]);
    break;
  case 52:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51]);
    break;
  case 53:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52]);
    break;
  case 54:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53]);
    break;
  case 55:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54]);
    break;
  case 56:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55]);
    break;
  case 57:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55],
            p_argv[56]);
    break;
  case 58:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55],
            p_argv[56], p_argv[57]);
    break;
  case 59:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55],
            p_argv[56], p_argv[57], p_argv[58]);
    break;
  case 60:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55],
            p_argv[56], p_argv[57], p_argv[58], p_argv[59]);
    break;
  case 61:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55],
            p_argv[56], p_argv[57], p_argv[58], p_argv[59], p_argv[60]);
    break;
  case 62:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55],
            p_argv[56], p_argv[57], p_argv[58], p_argv[59], p_argv[60],
            p_argv[61]);
    break;
  case 63:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55],
            p_argv[56], p_argv[57], p_argv[58], p_argv[59], p_argv[60],
            p_argv[61], p_argv[62]);
    break;
  case 64:
    (*pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2], p_argv[3], p_argv[4],
            p_argv[5], p_argv[6], p_argv[7], p_argv[8], p_argv[9], p_argv[10],
            p_argv[11], p_argv[12], p_argv[13], p_argv[14], p_argv[15],
            p_argv[16], p_argv[17], p_argv[18], p_argv[19], p_argv[20],
            p_argv[21], p_argv[22], p_argv[23], p_argv[24], p_argv[25],
            p_argv[26], p_argv[27], p_argv[28], p_argv[29], p_argv[30],
            p_argv[31], p_argv[32], p_argv[33], p_argv[34], p_argv[35],
            p_argv[36], p_argv[37], p_argv[38], p_argv[39], p_argv[40],
            p_argv[41], p_argv[42], p_argv[43], p_argv[44], p_argv[45],
            p_argv[46], p_argv[47], p_argv[48], p_argv[49], p_argv[50],
            p_argv[51], p_argv[52], p_argv[53], p_argv[54], p_argv[55],
            p_argv[56], p_argv[57], p_argv[58], p_argv[59], p_argv[60],
            p_argv[61], p_argv[62], p_argv[63]);
  }

#if OMPT_SUPPORT
  *exit_frame_ptr = 0;
#endif

  return 1;
}

#endif

#if KMP_USE_ABT

typedef struct kmp_abt {
  ABT_xstream *xstream;
  ABT_sched *sched;
  ABT_pool *priv_pool;
  ABT_pool *shared_pool;
  int num_xstreams;
  int num_pools;
} kmp_abt_t;

static kmp_abt_t *__kmp_abt = NULL;

static inline ABT_pool __kmp_abt_get_pool(int gtid) {
  KMP_DEBUG_ASSERT(__kmp_abt != NULL);
  KMP_DEBUG_ASSERT(gtid >= 0);

#if ABT_USE_PRIVATE_POOLS
  if (gtid < __kmp_abt->num_xstreams) {
    return __kmp_abt->priv_pool[gtid];
  } else {
    int eid = gtid % __kmp_abt->num_xstreams;
    return __kmp_abt->shared_pool[eid];
  }
#else
  int eid = gtid % __kmp_abt->num_xstreams;
  return __kmp_abt->shared_pool[eid];
#endif
}

static inline ABT_pool __kmp_abt_get_my_pool(int gtid) {
  if (gtid < __kmp_abt->num_xstreams) {
    return __kmp_abt->shared_pool[gtid];
  } else {
    int eid;
    ABT_xstream_self_rank(&eid);
    return __kmp_abt->shared_pool[eid];
  }
}

void __kmp_abt_release_info(kmp_info_t *th) {
  KMP_DEBUG_ASSERT(th->th.th_active == TRUE);
  TCW_4(th->th.th_active, FALSE);
}

void __kmp_abt_acquire_info_for_task(kmp_info_t *th,
                                     kmp_taskdata_t *taskdata) {
  while (KMP_COMPARE_AND_STORE_RET32(&th->th.th_active, FALSE, TRUE) != FALSE) {
    ABT_thread_yield();
  }
  th->th.th_current_task = taskdata;
}

void __kmp_abt_set_self_info(kmp_info_t *th) {
  ABT_thread self;

  KMP_ASSERT(__kmp_init_runtime);
  ABT_thread_self(&self);
  if (self == ABT_THREAD_NULL) {
    KMP_ASSERT(__kmp_init_gtid);
    /* External threads might call OpenMP functions. */
    int gtid = (size_t)pthread_getspecific(__kmp_gtid_threadprivate_key);
    KA_TRACE(50, ("__kmp_abt_set_self_info: key:%d gtid:%d\n",
                  __kmp_gtid_threadprivate_key, gtid));
    __kmp_threads[gtid] = th;
  } else {
    int ret = ABT_thread_set_arg(self, (void *)th);
    KMP_ASSERT(ret == ABT_SUCCESS);
  }
}

kmp_info_t *__kmp_abt_get_self_info(void) {
  ABT_thread self;

  KMP_ASSERT(__kmp_init_runtime);
  ABT_thread_self(&self);
  if (self == ABT_THREAD_NULL) {
    KMP_ASSERT(__kmp_init_gtid);
    /* External threads might call OpenMP functions. */
    int gtid = (size_t)pthread_getspecific(__kmp_gtid_threadprivate_key);
    KA_TRACE(50, ("__kmp_abt_get_self_info: key:%d gtid:%d\n",
                  __kmp_gtid_threadprivate_key, gtid));
    return __kmp_threads[gtid];
  } else {
    kmp_info_t *th;
    int ret = ABT_thread_get_arg(self, (void **)&th);
    KMP_ASSERT(th != NULL);
    KMP_ASSERT(ret == ABT_SUCCESS);
    return th;
  }
}

static void __kmp_abt_initialize(void) {
  int status;
  char *env;
  int num_xstreams, num_pools;
  int i, k;

  env = getenv("KMP_ABT_NUM_ESS");
  if (env) {
    num_xstreams = atoi(env);
    if (num_xstreams < __kmp_xproc)
      __kmp_xproc = num_xstreams;
  } else {
    num_xstreams = __kmp_xproc;
  }

  num_pools = num_xstreams;
  KA_TRACE(10, ("__kmp_abt_initialize: # of ESs = %d\n", num_xstreams));

  __kmp_abt = (kmp_abt_t *)__kmp_allocate(sizeof(kmp_abt_t));
  __kmp_abt->xstream = (ABT_xstream *)__kmp_allocate(num_xstreams
                                                     * sizeof(ABT_xstream));
  __kmp_abt->sched = (ABT_sched *)__kmp_allocate(num_xstreams
                                                 * sizeof(ABT_sched));
  __kmp_abt->priv_pool = (ABT_pool *)__kmp_allocate(num_pools
                                                    * sizeof(ABT_pool));
  __kmp_abt->shared_pool = (ABT_pool *)__kmp_allocate(num_pools
                                                      * sizeof(ABT_pool));
  __kmp_abt->num_xstreams = num_xstreams;
  __kmp_abt->num_pools = num_pools;

  /* Create pools */
  for (i = 0; i < num_xstreams; i++) {
#if ABT_USE_PRIVATE_POOLS
    status = ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPSC,
                                   ABT_TRUE, &__kmp_abt->priv_pool[i]);
    KMP_CHECK_SYSFAIL("ABT_pool_create_basic", status);
#endif
    status = ABT_pool_create_basic(ABT_POOL_FIFO, ABT_POOL_ACCESS_MPMC,
                                   ABT_TRUE, &__kmp_abt->shared_pool[i]);
    KMP_CHECK_SYSFAIL("ABT_pool_create_basic", status);
  }

  /* Create schedulers */
  ABT_sched_config_var cv_freq = {
    .idx = 0,
    .type = ABT_SCHED_CONFIG_INT
  };

  ABT_sched_config config;
  int freq = (num_xstreams < 100) ? 100 : num_xstreams;
  ABT_sched_config_create(&config, cv_freq, freq, ABT_sched_config_var_end);

  ABT_sched_def sched_def = {
    .type = ABT_SCHED_TYPE_ULT,
    .init = __kmp_abt_sched_init,
    .run = __kmp_abt_sched_run,
    .free = __kmp_abt_sched_free,
    .get_migr_pool = NULL
  };

  ABT_pool *my_pools;
  my_pools = (ABT_pool *)malloc((num_xstreams + 1) * sizeof(ABT_pool));

#if ABT_USE_PRIVATE_POOLS
  for (i = 0; i < num_xstreams; i++) {
    my_pools[0] = __kmp_abt->priv_pool[i];
    for (k = 0; k < num_xstreams; k++) {
      my_pools[k + 1] = __kmp_abt->shared_pool[(i + k) % num_xstreams];
    }
    status = ABT_sched_create(&sched_def, num_xstreams + 1, my_pools,
                              config, &__kmp_abt->sched[i]);
    KMP_CHECK_SYSFAIL("ABT_sched_create", status);
  }
#else /* ABT_USE_PRIVATE_POOLS */
  for (i = 0; i < num_xstreams; i++) {
    for (k = 0; k < num_xstreams; k++) {
      my_pools[k] = __kmp_abt->shared_pool[(i + k) % num_xstreams];
    }
    status = ABT_sched_create(&sched_def, num_xstreams, my_pools,
                              config, &__kmp_abt->sched[i]);
    KMP_CHECK_SYSFAIL("ABT_sched_create", status);
  }
#endif /* !ABT_USE_PRIVATE_POOLS */

  free(my_pools);
  ABT_sched_config_free(&config);

  /* Create ESs */
  status = ABT_xstream_self(&__kmp_abt->xstream[0]);
  KMP_CHECK_SYSFAIL("ABT_xstream_self", status);
  status = ABT_xstream_set_main_sched(__kmp_abt->xstream[0],
                                      __kmp_abt->sched[0]);
  KMP_CHECK_SYSFAIL("ABT_xstream_set_main_sched", status);
  for (i = 1; i < num_xstreams; i++) {
    status = ABT_xstream_create(__kmp_abt->sched[i], &__kmp_abt->xstream[i]);
    KMP_CHECK_SYSFAIL("ABT_xstream_create", status);
  }
}

static void __kmp_abt_finalize(void) {
  int status;
  int i;

  for (i = 1; i < __kmp_abt->num_xstreams; i++) {
    status = ABT_xstream_join(__kmp_abt->xstream[i]);
    KMP_CHECK_SYSFAIL("ABT_xstream_join", status);
    status = ABT_xstream_free(&__kmp_abt->xstream[i]);
    KMP_CHECK_SYSFAIL("ABT_xstream_free", status);
  }

  /* Free schedulers */
  for (i = 1; i < __kmp_abt->num_xstreams; i++) {
    status = ABT_sched_free(&__kmp_abt->sched[i]);
    KMP_CHECK_SYSFAIL("ABT_sched_free", status);
  }

  __kmp_free(__kmp_abt->xstream);
  __kmp_free(__kmp_abt->sched);
  __kmp_free(__kmp_abt->priv_pool);
  __kmp_free(__kmp_abt->shared_pool);
  __kmp_free(__kmp_abt);
  __kmp_abt = NULL;
}

volatile int __kmp_abt_init_global = FALSE;
void __kmp_abt_global_initialize() {
  int status;
  // Initialize Argobots before other initializations.
  status = ABT_init(0, NULL);
  KMP_CHECK_SYSFAIL("ABT_init", status);
  __kmp_abt_init_global = TRUE;
}

void __kmp_abt_global_destroy() {
  ABT_finalize();
  __kmp_abt_init_global = FALSE;
}

typedef struct {
  uint32_t event_freq;
} __kmp_abt_sched_data_t;

static int __kmp_abt_sched_init(ABT_sched sched, ABT_sched_config config) {
  __kmp_abt_sched_data_t *p_data = (__kmp_abt_sched_data_t *)
                                   calloc(1, sizeof(__kmp_abt_sched_data_t));
  ABT_sched_config_read(config, 1, &p_data->event_freq);
  ABT_sched_set_data(sched, (void *)p_data);
  return ABT_SUCCESS;
}

static void __kmp_abt_sched_run(ABT_sched sched) {
  uint32_t work_count = 0;
  __kmp_abt_sched_data_t *p_data;
  int num_pools, num_shared_pools;
  ABT_pool *pools;
  ABT_pool *shared_pools;
#if ABT_USE_PRIVATE_POOLS
  ABT_pool priv_pool;
#endif
  ABT_unit unit;
  unsigned seed = time(NULL);

#if ABT_USE_SCHED_SLEEP
  struct timespec sleep_time;
  sleep_time.tv_sec = 0;
  sleep_time.tv_nsec = 128;
#endif

  ABT_sched_get_data(sched, (void **)&p_data);
  ABT_sched_get_num_pools(sched, &num_pools);
  pools = (ABT_pool *)malloc(num_pools * sizeof(ABT_pool));
  ABT_sched_get_pools(sched, num_pools, 0, pools);

#if ABT_USE_PRIVATE_POOLS
  priv_pool = pools[0];
  shared_pools = pools + 1;
  num_shared_pools = num_pools - 1;
#else
  shared_pools = pools;
  num_shared_pools = num_pools;
#endif

  int sleep_cnt = 0;
  while (1) {
    int run_cnt = 0;
    size_t size;

#if ABT_USE_PRIVATE_POOLS
    /* Execute one work unit from the private pool */
    ABT_pool_get_size(priv_pool, &size);
    if (size != 0) {
      ABT_pool_pop(priv_pool, &unit);
      if (unit != ABT_UNIT_NULL) {
        ABT_xstream_run_unit(unit, priv_pool);
        run_cnt++;
      }
    }
#endif

    /* From the shared pool */
    ABT_pool_get_size(shared_pools[0], &size);
    if (size != 0) {
      ABT_pool_pop(shared_pools[0], &unit);
      if (unit != ABT_UNIT_NULL) {
        ABT_xstream_run_unit(unit, shared_pools[0]);
        run_cnt++;
      }
    }

    /* Steal a work unit from other pools */
    if (run_cnt == 0 && num_shared_pools >= 2) {
      int target = rand_r(&seed) % (num_shared_pools - 1) + 1;
      ABT_pool_get_size(shared_pools[target], &size);
      if (size != 0) {
        ABT_pool_pop(shared_pools[target], &unit);
        if (unit != ABT_UNIT_NULL) {
          ABT_unit_set_associated_pool(unit, shared_pools[0]);
          ABT_xstream_run_unit(unit, shared_pools[0]);
          run_cnt++;
        }
      }
    }

    if (++work_count >= p_data->event_freq) {
      ABT_bool stop;
      ABT_xstream_check_events(sched);
      ABT_sched_has_to_stop(sched, &stop);
      if (stop == ABT_TRUE)
        break;
      work_count = 0;
#if ABT_USE_SCHED_SLEEP
      if (run_cnt == 0) {
        sleep_cnt = 0;
        nanosleep(&sleep_time, NULL);
        if (sleep_time.tv_nsec < 1048576) {
          sleep_time.tv_nsec <<= 2;
        }
      } else if (sleep_cnt == 8) {
        sleep_cnt = 0;
        nanosleep(&sleep_time, NULL);
      } else {
        sleep_time.tv_nsec = 128;
        sleep_cnt++;
      }
#endif /* ABT_USE_SCHED_SLEEP */
    }
  }
  free(pools);
}

static int __kmp_abt_sched_free(ABT_sched sched) {
    __kmp_abt_sched_data_t *p_data;
    ABT_sched_get_data(sched, (void **)&p_data);
    free(p_data);
    return ABT_SUCCESS;
}

static inline void __kmp_abt_free_task(kmp_info_t *th, kmp_taskdata_t *taskdata)
{
  int gtid = __kmp_gtid_from_thread(th);

  KA_TRACE(30, ("__kmp_abt_free_task: (enter) T#%d - task %p\n",
                gtid, taskdata));

  /* [AC] we need those steps to mark the task as finished so the dependencies
   *  can be completed */
  taskdata->td_flags.complete = 1; // mark the task as completed
  __kmp_release_deps(gtid, taskdata);
  taskdata->td_flags.executing = 0; // suspend the finishing task

  // Wait for all tasks after releasing (=pushing) dependent tasks
  __kmp_abt_wait_child_tasks(th, FALSE);

  taskdata->td_flags.freed = 1;

  /* Free the task queue if it was allocated. */
  if (taskdata->td_task_queue) {
    KMP_DEBUG_ASSERT(taskdata->td_tq_cur_size == 0);
    KMP_INTERNAL_FREE(taskdata->td_task_queue);
  }

  // deallocate the taskdata and shared variable blocks associated with this
  // task
#if USE_FAST_MEMORY
  __kmp_fast_free(th, taskdata);
#else
  __kmp_thread_free(th, taskdata);
#endif

  KA_TRACE(30, ("__kmp_abt_free_task: (exit) T#%d - task %p\n",
                gtid, taskdata));
}

static void __kmp_abt_execute_task(void *arg) {
  // It is corresponding to __kmp_execute_tasks_.
  int gtid;

  kmp_task_t *task = (kmp_task_t *)arg;
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  kmp_info_t *th;

  th = __kmp_abt_bind_task_to_thread(taskdata->td_team, taskdata);
  gtid = __kmp_gtid_from_thread(th);

  KA_TRACE(20, ("__kmp_abt_execute_task: T#%d before executing task %p.\n",
                gtid, task));

  // Run __kmp_invoke_task to handle internal counters correctly.
#ifdef KMP_GOMP_COMPAT
  if (taskdata->td_flags.native) {
    ((void (*)(void *))(*(task->routine)))(task->shareds);
  } else
#endif /* KMP_GOMP_COMPAT */
  {
    (*(task->routine))(gtid, task);
  }

  if (!taskdata->td_flags.tiedness) {
    // If this task is an untied one, we need to retrieve kmp_info because it
    // may have been changed.
    th = __kmp_abt_get_self_info();
  }

  // Free this task.
  __kmp_abt_free_task(th, taskdata);

  // Reset th's ownership.
  __kmp_abt_release_info(th);

  KA_TRACE(20, ("__kmp_abt_execute_task: T#%d after executing task %p.\n",
                __kmp_gtid_from_thread(th), task));
}

int __kmp_abt_create_task(kmp_info_t *th, kmp_task_t *task) {
  int status;
  int gtid = __kmp_gtid_from_thread(th);
  ABT_pool dest = __kmp_abt_get_my_pool(gtid);

  KA_TRACE(20, ("__kmp_abt_create_task: T#%d before creating task %p into the "
                "pool %p.\n", gtid, task, dest));

  /* Check if the task queue has an empty slot. */
  kmp_taskdata_t *td = th->th.th_current_task;
  if (td->td_tq_cur_size == td->td_tq_max_size) {
    size_t new_max_size;
    if (td->td_tq_max_size == 0) {
      /* Empty queue. We allocate 32 slots by default. */
      new_max_size = 32;
    } else {
      /* The task queue is full. Expand it. */
      new_max_size = td->td_tq_max_size * 2;
    }

    void *queue = (void *)td->td_task_queue;
    size_t size = sizeof(kmp_abt_task_t) * new_max_size;
    td->td_task_queue = (kmp_abt_task_t *)KMP_INTERNAL_REALLOC(queue, size);
    td->td_tq_max_size = new_max_size;
  }

  status = ABT_thread_create(dest, __kmp_abt_execute_task, (void *)task,
                             ABT_THREAD_ATTR_NULL,
                             &td->td_task_queue[td->td_tq_cur_size++]);
  KMP_ASSERT(status == ABT_SUCCESS);

  KA_TRACE(20, ("__kmp_abt_create_task: T#%d after creating task %p into the "
                "pool %p.\n", gtid, task, dest));

  return TRUE;
}

void __kmp_abt_wait_child_tasks(kmp_info_t *th, int yield) {
  KA_TRACE(20, ("__kmp_abt_wait_child_tasks: T#%d enter\n",
                __kmp_gtid_from_thread(th)));

  int i, status;
  kmp_taskdata_t *taskdata = th->th.th_current_task;

  if (taskdata->td_tq_cur_size == 0) {
    /* leaf task case */
    if (yield) {
      __kmp_abt_release_info(th);

      ABT_thread_yield();

      if (taskdata->td_flags.tiedness) {
        __kmp_abt_acquire_info_for_task(th, taskdata);
      } else {
        __kmp_abt_bind_task_to_thread(th->th.th_team, taskdata);
      }
    }
    KA_TRACE(20, ("__kmp_abt_wait_child_tasks: T#%d done\n",
                  __kmp_gtid_from_thread(th)));
    return;
  }

  /* Let others, e.g., tasks, can use this kmp_info */
  __kmp_abt_release_info(th);

  /* Give other tasks a chance for execution */
  if (yield)
    ABT_thread_yield();

  /* Wait until all child tasks are complete. */
  for (i = 0; i < taskdata->td_tq_cur_size; i++) {
    status = ABT_thread_free(&taskdata->td_task_queue[i]);
    KMP_ASSERT(status == ABT_SUCCESS);
  }
  taskdata->td_tq_cur_size = 0;

  if (taskdata->td_flags.tiedness) {
    /* Obtain kmp_info to continue the original task. */
    __kmp_abt_acquire_info_for_task(th, taskdata);
  } else {
    th = __kmp_abt_bind_task_to_thread(th->th.th_team, taskdata);
  }

  KA_TRACE(20, ("__kmp_abt_wait_child_tasks: T#%d done\n",
                __kmp_gtid_from_thread(th)));
}

kmp_info_t *__kmp_abt_bind_task_to_thread(kmp_team_t *team,
                                          kmp_taskdata_t *taskdata) {
  int i, i_start, i_end;
  kmp_info_t *th = NULL;

  KA_TRACE(20, ("__kmp_abt_bind_task_to_thread: (enter) task %p\n", taskdata));

  /* To handle gtid in the task code, we look for a suspended (blocked)
   * thread in the team and use its info to execute this task. */
  while (1) {
    if (team->t.t_level <= 1) {
      /* outermost team - we try to assign the thread that was executed on
       * the same ES first and then check other threads in the team.  */
      int rank;
      ABT_xstream_self_rank(&rank);
      if (rank < team->t.t_nproc) {
        /* [SM] I think this condition should always be true, but just in
         * case I miss something we check this condition. */
        i_start = rank;
        i_end = team->t.t_nproc + rank;
      } else {
        i_start = 0;
        i_end = team->t.t_nproc;
      }
    } else {
      /* nested team - we ignore the ES info since threads in the nested team
       * may be executed by any ES. */
      i_start = 0;
      i_end = team->t.t_nproc;
    }
    /* TODO: This is a linear search. Can we do better? */
    for (i = i_start; i < i_end; i++) {
      int idx = (i < team->t.t_nproc) ? i : i % team->t.t_nproc;
      th = team->t.t_threads[idx];
      ABT_thread ult = th->th.th_info.ds.ds_thread;

      if (th->th.th_active == FALSE && ult != ABT_THREAD_NULL) {
        /* Try to take the ownership of kmp_info 'th' */
        if (KMP_COMPARE_AND_STORE_RET32(&th->th.th_active, FALSE, TRUE)
            == FALSE) {
          /* Bind this task as if it is executed by 'th'. */
          th->th.th_current_task = taskdata;
          __kmp_abt_set_self_info(th);
          KA_TRACE(20, ("__kmp_abt_bind_task_to_thread: (exit) task %p"
                        "bound to T#%d\n",
                        taskdata, __kmp_gtid_from_thread(th)));
          return th;
        }
      }
    }
    /* We could not find an available kmp_info. Thus, this task yields
     * control to other work units and will try to find one later. */
    ABT_thread_yield();
  }
  return NULL;
}

void __kmp_abt_create_uber(int gtid, kmp_info_t *th, size_t stack_size) {
  KMP_DEBUG_ASSERT(KMP_UBER_GTID(gtid));
  KA_TRACE(10, ("__kmp_abt_create_uber: T#%d\n", gtid));
  ABT_thread handle;
  ABT_thread_self(&handle);
  if (handle == ABT_THREAD_NULL) {
    // External threads might call this function.  In this case, we do not need
    // to set `th` since external threads use pthread_setspecific,
    __kmp_gtid_set_specific(gtid);
  } else {
    ABT_thread_set_arg(handle, (void *)th);
  }
  th->th.th_info.ds.ds_thread = handle;
}

#endif // KMP_USE_ABT

// end of file //
