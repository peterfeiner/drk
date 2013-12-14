/* **********************************************************
 * Copyright (c) 2011-2013 Google, Inc.  All rights reserved.
 * Copyright (c) 2008-2010 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
 * signal_private.h - declarations shared among signal handling files, but not
 * exported to the rest of the code
 */

#ifndef _SIGNAL_PRIVATE_H_
#define _SIGNAL_PRIVATE_H_ 1

/* We have an ordering issue so we split out LINUX from globals.h */
#include "configure.h"

#ifdef LINUX
/* We want to build on older toolchains so we have our own copy of signal
 * data structures
 */
#  include "include/sigcontext.h"
#  include "include/signalfd.h"
#  include "../globals.h" /* after our sigcontext.h, to preclude bits/sigcontext.h */
#elif defined(MACOS)
#  include "../globals.h" /* this defines _XOPEN_SOURCE for Mac */
#  include <signal.h> /* after globals.h, for _XOPEN_SOURCE from os_exports.h */
#endif

#include "os_private.h"

/***************************************************************************
 * MISC DEFINITIONS
 */

/* handler with SA_SIGINFO flag set gets three arguments: */
typedef void (*handler_t)(int, siginfo_t *, void *);

/* default actions */
enum {
    DEFAULT_TERMINATE,
    DEFAULT_TERMINATE_CORE,
    DEFAULT_IGNORE,
    DEFAULT_STOP,
    DEFAULT_CONTINUE,
};

/* even though we don't execute xsave ourselves, kernel will do xrestore on sigreturn
 * so we have to obey alignment for avx
 */
#define AVX_ALIGNMENT 64
#define FPSTATE_ALIGNMENT 16
#define XSTATE_ALIGNMENT (YMM_ENABLED() ? AVX_ALIGNMENT : FPSTATE_ALIGNMENT)

/***************************************************************************
 * FRAMES
 */

/* kernel's notion of sigaction has fields in different order from that
 * used in glibc (I looked at glibc-2.2.4/sysdeps/unix/sysv/linux/i386/sigaction.c)
 * also, /usr/include/asm/signal.h lists both versions
 * I deliberately give kernel_sigaction_t's fields different names to help
 * avoid confusion.
 * (2.1.20 kernel has mask as 2nd field instead, and is expected to be passed
 * in to the non-rt sigaction() call, which we do not yet support)
 */
struct _kernel_sigaction_t {
    handler_t handler;
#ifdef LINUX
    unsigned long flags;
    void (*restorer)(void);
    kernel_sigset_t mask;
#elif defined(MACOS)
    /* this is struct __sigaction in sys/signal.h */
    void (*restorer)(void);
    kernel_sigset_t mask;
    int flags;
#endif
}; /* typedef in os_private.h */

#ifdef LINUX
/* kernel's notion of ucontext is different from glibc's!
 * this is adapted from asm/ucontext.h:
 */
typedef struct {
    unsigned long     uc_flags;
    struct ucontext  *uc_link;
    stack_t           uc_stack;
    sigcontext_t      uc_mcontext;
    kernel_sigset_t   uc_sigmask; /* mask last for extensibility */
} kernel_ucontext_t;

#  define SIGCXT_FROM_UCXT(ucxt) (&((ucxt)->uc_mcontext))
#  define SIGMASK_FROM_UCXT(ucxt) (&((ucxt)->uc_sigmask))

#elif defined(MACOS)
#  ifdef X64
typedef _STRUCT_UCONTEXT64 /* == __darwin_ucontext64 */ kernel_ucontext_t;
#    define SIGCXT_FROM_UCXT(ucxt) ((ucxt)->uc_mcontext64)
#  else
typedef _STRUCT_UCONTEXT /* == __darwin_ucontext */ kernel_ucontext_t;
#    define SIGCXT_FROM_UCXT(ucxt) ((ucxt)->uc_mcontext)
#  endif
#  define SIGMASK_FROM_UCXT(ucxt) ((kernel_sigset_t*)&((ucxt)->uc_sigmask))
#endif

#ifdef LINUX
/* we assume frames look like this, with rt_sigframe used if SA_SIGINFO is set
 * (these are from /usr/src/linux/arch/i386/kernel/signal.c for kernel 2.4.17)
 */

#define RETCODE_SIZE 8

typedef struct sigframe {
    char *pretcode;
    int sig;
    sigcontext_t sc;
    /* Since 2.6.28, this fpstate has been unused and the real fpstate
     * is at the end of the struct so it can include xstate
     */
    struct _fpstate fpstate;
    unsigned long extramask[_NSIG_WORDS-1];
    char retcode[RETCODE_SIZE];
    /* FIXME: this is a field I added, so our frame looks different from
     * the kernel's...but where else can I store sig where the app won't
     * clobber it?
     * WARNING: our handler receives only rt frames, and we construct
     * plain frames but never pass them to the kernel (on sigreturn() we
     * just go to new context and interpret from there), so the only
     * transparency problem here is if app tries to build its own plain
     * frame and call sigreturn() unrelated to signal delivery.
     * UPDATE: actually we do invoke SYS_*sigreturn
     */
    int sig_noclobber;
    /* In 2.6.28+, fpstate/xstate goes here */
} sigframe_plain_t;
#else
/* Mac only has one frame type, with a libc stub that calls 1-arg or 3-arg handler */
#endif

/* the rt frame is used for SA_SIGINFO signals */
typedef struct rt_sigframe {
#ifdef LINUX
    char *pretcode;
#  ifdef X64
#    ifdef VMX86_SERVER
    siginfo_t info;
    kernel_ucontext_t uc;
#     else
    kernel_ucontext_t uc;
    siginfo_t info;
#     endif
#  else
    int sig;
    siginfo_t *pinfo;
    void *puc;
    siginfo_t info;
    kernel_ucontext_t uc;
    /* Prior to 2.6.28, "struct _fpstate fpstate" was here.  Rather than
     * try to reproduce that exact layout and detect the underlying kernel
     * (the safest way would be to send ourselves a signal and examine the
     * frame, rather than relying on uname, to handle backports), we use
     * the new layout even on old kernels.  The app should use the fpstate
     * pointer in the sigcontext anyway.
     */
    char retcode[RETCODE_SIZE];
#  endif
    /* In 2.6.28+, fpstate/xstate goes here */

#elif defined(MACOS)
#  ifdef X64
    /* kernel places padding to align to 16, and then puts retaddr slot */
    struct __darwin_mcontext_avx64 mc; /* "struct mcontext_avx64" to kernel */
    siginfo_t info; /* matches user-mode sys/signal.h struct */
    struct __darwin_ucontext64 uc; /* "struct user_ucontext64" to kernel */
#  else
    app_pc retaddr;
    app_pc handler;
    int sigstyle; /* UC_TRAD = 1-arg, UC_FLAVOR = 3-arg handler */
    int sig;
    siginfo_t *pinfo;
    struct __darwin_ucontext *puc; /* "struct user_ucontext32 *" to kernel */
    struct __darwin_mcontext_avx32 mc; /* "struct mcontext_avx32" to kernel */
    siginfo_t info; /* matches user-mode sys/signal.h struct */
    struct __darwin_ucontext uc; /* "struct user_ucontext32" to kernel */
#  endif
#endif
} sigframe_rt_t;

/* we have to queue up both rt and non-rt signals because we delay
 * their delivery.
 * PR 304708: we now leave in rt form right up until we copy to the
 * app stack, so that we can deliver to a client at a safe spot
 * in rt form.
 */
typedef struct _sigpending_t {
    sigframe_rt_t rt_frame;
#ifdef LINUX
    /* fpstate is no longer kept inside the frame, and is not always present.
     * if we delay we need to ensure we have room for it.
     * we statically keep room for full xstate in case we need it.
     */
    struct _xstate __attribute__ ((aligned (AVX_ALIGNMENT))) xstate;
#endif
#ifdef CLIENT_INTERFACE
    /* i#182/PR 449996: we provide the faulting access address for SIGSEGV, etc. */
    byte *access_address;
#endif
    /* use the sigcontext, not the mcontext (used to restart syscalls for i#1145) */
    bool use_sigcontext;
    /* was this unblocked at receive time? */
    bool unblocked;
    struct _sigpending_t *next;
} sigpending_t;

/***************************************************************************
 * PER-THREAD DATA
 */

/* PR 204556: DR/clients use itimers so we need to emulate the app's usage */
typedef struct _itimer_info_t {
    /* easier to manipulate a single value than the two-field struct timeval */
    uint64 interval;
    uint64 value;
} itimer_info_t;

typedef struct _thread_itimer_info_t {
    itimer_info_t app;
    itimer_info_t app_saved;
    itimer_info_t dr;
    itimer_info_t actual;
    void (*cb)(dcontext_t *, priv_mcontext_t *);
    /* version for clients */
    void (*cb_api)(dcontext_t *, dr_mcontext_t *);
} thread_itimer_info_t;

/* We use all 3: ITIMER_REAL for clients (i#283/PR 368737), ITIMER_VIRTUAL
 * for -prof_pcs, and ITIMER_PROF for PAPI
 */
#define NUM_ITIMERS 3

/* Don't try to translate every alarm if they're piling up (PR 213040) */
#define SKIP_ALARM_XL8_MAX 3

struct _sigfd_pipe_t;
typedef struct _sigfd_pipe_t sigfd_pipe_t;

typedef struct _thread_sig_info_t {
    /* we use kernel_sigaction_t so we don't have to translate back and forth
     * between it and libc version.
     * have to dynamically allocate app_sigaction array so we can share it.
     */
    kernel_sigaction_t **app_sigaction;

    /* True after signal_thread_inherit or signal_fork_init are called.  We
     * squash alarm or profiling signals up until this point.
     */
    bool fully_initialized;

    /* with CLONE_SIGHAND we may have to share app_sigaction */
    bool shared_app_sigaction;
    mutex_t *shared_lock;
    int *shared_refcount;
    /* signals we intercept must also be sharable */
    bool *we_intercept;

    /* DR and clients use itimers, so we need to emulate the app's itimer
     * usage.  This info is shared across CLONE_THREAD threads only for
     * NPTL in kernel 2.6.12+ so these fields are separately shareable from
     * the CLONE_SIGHAND set of fields above.
     */
    bool shared_itimer;
    /* We only need owner info.  xref i#219: we should add a known-owner
     * lock for cases where a full-fledged recursive lock is not needed.
     */
    recursive_lock_t *shared_itimer_lock;
    /* b/c a non-CLONE_THREAD thread can be created we can't just use dynamo_exited
     * and need a refcount here
     */
    int *shared_itimer_refcount;
    int *shared_itimer_underDR; /* indicates # of threads under DR control */
    thread_itimer_info_t (*itimer)[NUM_ITIMERS];

    /* cache restorer validity.  not shared: inheriter will re-populate. */
    int restorer_valid[SIGARRAY_SIZE];

    /* rest of app state */
    stack_t app_sigstack;
    sigpending_t *sigpending[SIGARRAY_SIZE];
    /* "lock" to prevent interrupting signal from messing up sigpending array */
    bool accessing_sigpending;
    kernel_sigset_t app_sigblocked;
    /* for returning the old mask (xref PR 523394) */
    kernel_sigset_t pre_syscall_app_sigblocked;
    /* for preserving the app memory (xref i#1187) */
    kernel_sigset_t pre_syscall_app_sigprocmask;
    /* for alarm signals arriving in coarse units we only attempt to xl8
     * every nth signal since coarse translation is expensive (PR 213040)
     */
    uint skip_alarm_xl8;
    /* signalfd array (lazily initialized) */
    sigfd_pipe_t *signalfd[SIGARRAY_SIZE];

    /* to handle sigsuspend we have to save blocked set */
    bool in_sigsuspend;
    kernel_sigset_t app_sigblocked_save;

    /* to inherit in children must not modify until they're scheduled */
    volatile int num_unstarted_children;
    mutex_t child_lock;

    /* our own structures */
    stack_t sigstack;
    void *sigheap; /* special heap */
    fragment_t *interrupted; /* frag we unlinked for delaying signal */
    cache_pc interrupted_pc; /* pc within frag we unlinked for delaying signal */

#ifdef RETURN_AFTER_CALL
    app_pc signal_restorer_retaddr;     /* last signal restorer, known ret exception */
#endif
} thread_sig_info_t;

/***************************************************************************
 * GENERAL ROUTINES (in signal.c)
 */

sigcontext_t *
get_sigcontext_from_rt_frame(sigframe_rt_t *frame);

/**** kernel_sigset_t ***************************************************/

/* defines and typedefs are exported in os_exports.h for siglongjmp */

/* For MacOS, the type is really __darwin_sigset_t, which is a plain __uint32_t.
 * We stick with the struct-containing-uint to simplify the helpers here.
 */

/* most of these are from /usr/src/linux/include/linux/signal.h */
static inline 
void kernel_sigemptyset(kernel_sigset_t *set)
{
    memset(set, 0, sizeof(kernel_sigset_t));
}

static inline 
void kernel_sigfillset(kernel_sigset_t *set)
{
    memset(set, -1, sizeof(kernel_sigset_t));
}

static inline 
void kernel_sigaddset(kernel_sigset_t *set, int _sig)
{
    uint sig = _sig - 1;
    if (_NSIG_WORDS == 1)
        set->sig[0] |= 1UL << sig;
    else
        set->sig[sig / _NSIG_BPW] |= 1UL << (sig % _NSIG_BPW);
}

static inline 
void kernel_sigdelset(kernel_sigset_t *set, int _sig)
{
    uint sig = _sig - 1;
    if (_NSIG_WORDS == 1)
        set->sig[0] &= ~(1UL << sig);
    else
        set->sig[sig / _NSIG_BPW] &= ~(1UL << (sig % _NSIG_BPW));
}

static inline 
bool kernel_sigismember(kernel_sigset_t *set, int _sig)
{
    int sig = _sig - 1; /* go to 0-based */
    if (_NSIG_WORDS == 1)
        return CAST_TO_bool(1 & (set->sig[0] >> sig));
    else
        return CAST_TO_bool(1 & (set->sig[sig / _NSIG_BPW] >> (sig % _NSIG_BPW)));
}

/* FIXME: how does libc do this? */
static inline
void copy_kernel_sigset_to_sigset(kernel_sigset_t *kset, sigset_t *uset)
{
    int sig;
#ifdef DEBUG
    int rc =
#endif 
        sigemptyset(uset);
    ASSERT(rc == 0);
    /* do this the slow way...I don't want to make assumptions about
     * structure of user sigset_t
     */
    for (sig=1; sig<=MAX_SIGNUM; sig++) {
        if (kernel_sigismember(kset, sig))
            sigaddset(uset, sig);
    }
}

/* FIXME: how does libc do this? */
static inline void
copy_sigset_to_kernel_sigset(sigset_t *uset, kernel_sigset_t *kset)
{
    int sig;
    kernel_sigemptyset(kset);
    /* do this the slow way...I don't want to make assumptions about
     * structure of user sigset_t
     */
    for (sig=1; sig<=MAX_SIGNUM; sig++) {
        if (sigismember(uset, sig))
            kernel_sigaddset(kset, sig);
    }
}

/***************************************************************************
 * OS-SPECIFIC ROUTINES (in signal_<os>.c)
 */

void
sigcontext_to_mcontext_mm(priv_mcontext_t *mc, sigcontext_t *sc);

void
mcontext_to_sigcontext_mm(sigcontext_t *sc, priv_mcontext_t *mc);

void
save_fpstate(dcontext_t *dcontext, sigframe_rt_t *frame);

#ifdef DEBUG
void
dump_sigcontext(dcontext_t *dcontext, sigcontext_t *sc);
#endif

#ifdef LINUX
void
signalfd_init(void);

void
signalfd_exit(void);

void
signalfd_thread_exit(dcontext_t *dcontext, thread_sig_info_t *info);

bool
notify_signalfd(dcontext_t *dcontext, thread_sig_info_t *info, int sig,
                sigframe_rt_t *frame);
#endif


#endif /* _SIGNAL_PRIVATE_H_ */