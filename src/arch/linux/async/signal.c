#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <assert.h>
#ifdef __linux__
#include <sys/prctl.h>
#include <linux/version.h>
#endif

#include "emu.h"
#ifdef __linux__
#include "sys_vm86.h"
#endif
#include "bios.h"
#include "mouse.h"
#include "video.h"
#include "vgaemu.h"
#include "vgatext.h"
#include "render.h"
#include "timers.h"
#include "int.h"
#include "lowmem.h"
#include "coopth.h"
#include "emudpmi.h"
#include "dnative.h"
#include "pic.h"
#include "ipx.h"
#include "pktdrvr.h"
#include "iodev.h"
#include "serial.h"
#include "debug.h"
#include "mhpdbg.h"
#include "utilities.h"
#include "ringbuf.h"
#include "dosemu_config.h"
#include "sound.h"
#include "cpu-emu.h"
#include "sig.h"

/* Variables for keeping track of signals */
#define MAX_SIG_QUEUE_SIZE 50
#define MAX_SIG_DATA_SIZE 128
static u_short SIGNAL_head=0; u_short SIGNAL_tail=0;
struct  SIGNAL_queue {
  void (*signal_handler)(void *);
  char arg[MAX_SIG_DATA_SIZE];
  size_t arg_size;
  const char *name;
};
static struct SIGNAL_queue signal_queue[MAX_SIG_QUEUE_SIZE];

#define MAX_SIGCHLD_HANDLERS 10
struct sigchld_hndl {
  pid_t pid;
  void (*handler)(void*);
  void *arg;
  int enabled;
};
static struct sigchld_hndl chld_hndl[MAX_SIGCHLD_HANDLERS];
static int chd_hndl_num;

#define MAX_SIGALRM_HANDLERS 50
struct sigalrm_hndl {
  void (*handler)(void);
};
static struct sigalrm_hndl alrm_hndl[MAX_SIGALRM_HANDLERS];
static int alrm_hndl_num;

struct callback_s {
  void (*func)(void *);
  void *arg;
  const char *name;
};

sigset_t q_mask;
sigset_t nonfatal_q_mask;
static sigset_t fatal_q_mask;
static int sig_inited;

static int sh_tid;
static int in_handle_signals;
static void handle_signals_force_enter(int tid, int sl_state);
static void handle_signals_force_leave(int tid, void *arg, void *arg2);
static void async_call(void *arg);
static void process_callbacks(void);
static struct rng_s cbks;
#define MAX_CBKS 1000
static pthread_mutex_t cbk_mtx = PTHREAD_MUTEX_INITIALIZER;

struct eflags_fs_gs eflags_fs_gs;

static void (*sighandlers[NSIG])(sigcontext_t *, siginfo_t *);
static void (*qsighandlers[NSIG])(int sig, siginfo_t *si, void *uc);
static void (*asighandlers[NSIG])(void *arg);

static void SIGALRM_call(void *arg);
static void SIGIO_call(void *arg);
static void sigasync(int sig, siginfo_t *si, void *uc);
static void sigasync_std(int sig, siginfo_t *si, void *uc);

static void _newsetqsig(int sig, void (*fun)(int sig, siginfo_t *si, void *uc))
{
	if (qsighandlers[sig])
		return;
	/* collect this mask so that all async (fatal and non-fatal)
	 * signals can be blocked by threads */
	sigaddset(&q_mask, sig);
	qsighandlers[sig] = fun;
}

static void newsetqsig(int sig, void (*fun)(int sig, siginfo_t *si, void *uc))
{
	sigaddset(&fatal_q_mask, sig);
	_newsetqsig(sig, fun);
}

static void init_one_sig(int num, void (*fun)(int sig, siginfo_t *si, void *uc))
{
	struct sigaction sa;
#ifdef DNATIVE
	sa.sa_flags = SA_RESTART | SA_ONSTACK | SA_SIGINFO;
	if (signative_block_all_sigs())
	{
		/* initially block all async signals. */
		sa.sa_mask = q_mask;
	}
	else
	{
		/* block all non-fatal async signals */
		sa.sa_mask = nonfatal_q_mask;
	}
#else
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sa.sa_mask = nonfatal_q_mask;
#endif
	sa.sa_sigaction = fun;
	sigaction(num, &sa, NULL);
}

static void qsig_init(void)
{
	int i;
	for (i = 0; i < NSIG; i++) {
		if (qsighandlers[i])
			init_one_sig(i, qsighandlers[i]);
	}
}

static void setup_nf_sig(int sig)
{
	/* first need to collect the mask, then register all handlers
	 * because the same mask of non-emergency async signals
	 * is used for every handler. */
	sigaddset(&nonfatal_q_mask, sig);
	/* Also we block them all until init is completed.  */
	sigaddset(&q_mask, sig);
}

static void do_registersig(int sig, void (*fun)(int sig, siginfo_t *si, void *uc))
{
	assert(!sig_inited);
	sigaddset(&nonfatal_q_mask, sig);
	_newsetqsig(sig, fun);
}

/* registers non-emergency async signals */
void registersig(int sig, void (*fun)(sigcontext_t *, siginfo_t *))
{
	assert(fun && !sighandlers[sig]);
	sighandlers[sig] = fun;
	do_registersig(sig, sigasync);
}

void registersig_std(int sig, void (*fun)(void *))
{
	assert(fun && !asighandlers[sig]);
	asighandlers[sig] = fun;
	do_registersig(sig, sigasync_std);
}

static void newsetsig(int sig, void (*fun)(int sig, siginfo_t *si, void *uc))
{
	struct sigaction sa;

	sa.sa_flags = SA_SIGINFO;
	/* block all non-fatal async signals */
	sa.sa_mask = nonfatal_q_mask;
	sa.sa_sigaction = fun;
	sigaction(sig, &sa, NULL);
}

/* init_handler puts the handler in a sane state that glibc
   expects. That means restoring fs and gs for vm86 (necessary for
   2.4 kernels) and fs, gs and eflags for DPMI. */
SIG_PROTO_PFX
static void __init_handler(sigcontext_t *scp, unsigned long uc_flags)
{
  /*
   * FIRST thing to do in signal handlers - to avoid being trapped into int0x11
   * forever, we must restore the eflags.
   */
  loadflags(eflags_fs_gs.eflags);

#ifdef __x86_64__
  /* ds,es, and ss are ignored in 64-bit mode and not present or
     saved in the sigcontext, so we need to do it ourselves
     (using the 3 high words of the trapno field).
     fs and gs are set to 0 in the sigcontext, so we also need
     to save those ourselves */
  _scp_ds = getsegment(ds);
  _scp_es = getsegment(es);
  if (!signative_skip_ss(uc_flags))
    _scp_ss = getsegment(ss);
  _scp_fs = getsegment(fs);
  _scp_gs = getsegment(gs);
  if (config.cpu_vm_dpmi == CPUVM_NATIVE && _scp_cs == 0) {
      if (config.dpmi
#ifdef X86_EMULATOR
	    && !EMU_DPMI()
#endif
	 ) {
	fprintf(stderr, "Cannot run DPMI code natively ");
#ifdef __linux__
	if (kernel_version_code < KERNEL_VERSION(2, 6, 15))
	  fprintf(stderr, "because your Linux kernel is older than version 2.6.15.\n");
	else
	  fprintf(stderr, "for unknown reasons.\nPlease contact linux-msdos@vger.kernel.org.\n");
#endif
#ifdef X86_EMULATOR
	fprintf(stderr, "Set $_cpu_emu=\"full\" or \"fullsim\" to avoid this message.\n");
#endif
      }
#ifdef X86_EMULATOR
      config.cpu_vm = CPUVM_EMU;
      _scp_cs = getsegment(cs);
#else
      leavedos_sig(45);
#endif
  }
#endif

  if (in_vm86) {
#ifdef __i386__
#ifdef X86_EMULATOR
    if (config.cpu_vm != CPUVM_EMU)
#endif
      {
	if (getsegment(fs) != eflags_fs_gs.fs)
	  loadregister(fs, eflags_fs_gs.fs);
	if (getsegment(gs) != eflags_fs_gs.gs)
	  loadregister(gs, eflags_fs_gs.gs);
      }
#endif
    return;
  }

  signative_enter(scp);
}

SIG_PROTO_PFX
void init_handler(sigcontext_t *scp, unsigned long uc_flags)
{
  /* Async signals are initially blocked.
   * If we don't block them, nested sighandler will clobber SS
   * before we manage to save it.
   * Even if the nested sighandler tries hard, it can't properly
   * restore SS, at least until the proper sigreturn() support is in.
   * For kernels that have the proper SS support, only nonfatal
   * async signals are initially blocked. They need to be blocked
   * because of sas wa and because they should not interrupt
   * deinit_handler() after it changed %fs. In this case, however,
   * we can block them later at the right places, but this will
   * cost a syscall per every signal.
   * Note: in 64bit mode some segment registers are neither saved nor
   * restored by the signal dispatching code in kernel, so we have
   * to restore them by hands.
   * Note: most async signals are left blocked, we unblock only few.
   * Sync signals like SIGSEGV are never blocked.
   */
  __init_handler(scp, uc_flags);
  if (!signative_block_all_sigs())
    return;
  if (signative_skip_unblock(scp))
    return;
  /* either came from dosemu/vm86 or having SS_AUTODISARM -
   * then we can unblock any signals we want. This is because
   * dosemu DOES NOT USE signal stack by itself. We switch to
   * dosemu via a direct context switch (including a stack switch),
   * before which we make sure either SS_AUTODISARM or sas_wa worked.
   * So we are here either on dosemu stack, or on autodisarmed SAS.
   * For now leave nonfatal signals blocked as they are rarely needed
   * inside sighandlers (needed only for instremu, see
   * https://github.com/stsp/dosemu2/issues/477
   * ) */
  sigprocmask(SIG_UNBLOCK, &fatal_q_mask, NULL);
}

SIG_PROTO_PFX
void deinit_handler(sigcontext_t *scp, unsigned long *uc_flags)
{
#ifdef X86_EMULATOR
  /* in fullsim mode nothing to do */
  if (CONFIG_CPUSIM && EMU_DPMI())
    return;
#endif

  signative_leave(scp, uc_flags);
}

static void leavedos_call(void *arg)
{
  int *sig = arg;
  _leavedos_sig(*sig);
}

int sigchld_register_handler(pid_t pid, void (*handler)(void*), void *arg)
{
  int i;
  for (i = 0; i < chd_hndl_num; i++) {
    if (!chld_hndl[i].pid)
      break;
    /* make sure not yet registered */
    assert(chld_hndl[i].pid != pid);
  }
  if (i == chd_hndl_num) {
    if (chd_hndl_num >= MAX_SIGCHLD_HANDLERS) {
      error("too many sigchld handlers\n");
      return -1;
    }
    chd_hndl_num++;
  }
  chld_hndl[i].handler = handler;
  chld_hndl[i].arg = arg;
  chld_hndl[i].pid = pid;
  chld_hndl[i].enabled = 1;
  return 0;
}

int sigchld_enable_cleanup(pid_t pid)
{
  return sigchld_register_handler(pid, NULL, NULL);
}

int sigchld_enable_handler(pid_t pid, int on)
{
  int i;
  for (i = 0; i < chd_hndl_num; i++) {
    if (chld_hndl[i].pid == pid)
      break;
  }
  if (i >= chd_hndl_num)
    return -1;
  chld_hndl[i].enabled = on;
  return 0;
}

static void cleanup_child(void *arg)
{
  int i, status;
  pid_t pid2, pid = *(pid_t *)arg;

  for (i = 0; i < chd_hndl_num; i++) {
    if (chld_hndl[i].pid == pid)
      break;
  }
  if (i >= chd_hndl_num)
    return;
  if (!chld_hndl[i].enabled)
    return;
  pid2 = waitpid(pid, &status, WNOHANG);
  if (pid2 != pid)
    return;
  /* SIGCHLD handler is one-shot, disarm */
  chld_hndl[i].pid = 0;
  if (chld_hndl[i].handler)
    chld_hndl[i].handler(chld_hndl[i].arg);
}

static void sigbreak(sigcontext_t *scp)
{
  if (!in_vm86) {
    switch (config.cpu_vm_dpmi) {
      case CPUVM_NATIVE:
#ifdef DNATIVE
        if (DPMIValidSelector(_scp_cs))
          dpmi_return(scp, DPMI_RET_DOSEMU);
#endif
        break;
      case CPUVM_EMU:
        /* compiled code can't check signal_pending() so we hint it */
        e_gen_sigalrm();
        break;
      case CPUVM_KVM:
        /* nothing, kvm.c checks signal_pending() */
        break;
    }
  } else if (config.cpu_vm == CPUVM_EMU) {
    e_gen_sigalrm();
  }
}

/* this cleaning up is necessary to avoid the port server becoming
   a zombie process */
static void sig_child(sigcontext_t *scp, siginfo_t *si)
{
  SIGNAL_save(cleanup_child, &si->si_pid, sizeof(si->si_pid), __func__);
}

int sigalrm_register_handler(void (*handler)(void))
{
  assert(alrm_hndl_num < MAX_SIGALRM_HANDLERS);
  alrm_hndl[alrm_hndl_num].handler = handler;
  alrm_hndl_num++;
  return 0;
}

void leavedos_from_sig(int sig)
{
  /* anything more sophisticated? */
  _leavedos_main(0, sig);
}

void leavedos_sig(int sig)
{
  /* do not log anything from a sighandler or it may hang */
  SIGNAL_save(leavedos_call, &sig, sizeof(sig), __func__);
  /* abort current sighandlers */
  if (in_handle_signals) {
    in_handle_signals = 0;
  }
}

/* noinline is needed to prevent gcc from caching tls vars before
 * calling to init_handler() */
__attribute__((noinline))
static void _leavedos_signal(int sig, sigcontext_t *scp)
{
  leavedos_sig(sig);
  sigbreak(scp);
}

SIG_PROTO_PFX
static void leavedos_signal(int sig, siginfo_t *si, void *uc)
{
  ucontext_t *uct = uc;
  sigcontext_t *scp = &uct->uc_mcontext;
  init_handler(scp, uct->uc_flags);
  signal(sig, SIG_DFL);
  _leavedos_signal(sig, scp);
  deinit_handler(scp, &uct->uc_flags);
}

#if 0
/* calling leavedos directly from sighandler may be unsafe - disable */
SIG_PROTO_PFX
static void leavedos_emerg(int sig, siginfo_t *si, void *uc)
{
  ucontext_t *uct = uc;
  sigcontext_t *scp = &uct->uc_mcontext;
  init_handler(scp, uct->uc_flags);
  leavedos_from_sig(sig);
  deinit_handler(scp, &uct->uc_flags);
}
#endif

SIG_PROTO_PFX
static void abort_signal(int sig, siginfo_t *si, void *uc)
{
  gdb_debug();
  _exit(sig);
}

SIG_PROTO_PFX
static void minfault(int sig, siginfo_t *si, void *uc)
{
#if defined(__i386__) || defined(X86_EMULATOR)
  ucontext_t *uct = uc;
  sigcontext_t *scp = &uct->uc_mcontext;
#ifdef __i386__
  if (in_vm86 && config.cpu_vm == CPUVM_VM86) {
    true_vm86_fault(scp);
    return;
  }
#endif
#ifdef X86_EMULATOR
  if (IS_EMU() && e_emu_fault(scp, in_vm86))
    return;
#endif
#endif
  gdb_debug();
  _exit(sig);
}

/* Silly Interrupt Generator Initialization/Closedown */

#ifdef SIG
SillyG_t       *SillyG = 0;
static SillyG_t SillyG_[16 + 1];
#endif

/*
 * DANG_BEGIN_FUNCTION SIG_init
 *
 * description: Allow DOSEMU to be made aware when a hard interrupt occurs
 * The IRQ numbers to monitor are taken from config.sillyint, each bit
 * corresponding to one IRQ. The higher 16 bit are defining the use of
 * SIGIO
 *
 * DANG_END_FUNCTION
 */
void SIG_init(void)
{
#if defined(SIG)
    PRIV_SAVE_AREA
    /* Get in touch with Silly Interrupt Handling */
    if (config.sillyint) {
	char            prio_table[] =
	{8, 9, 10, 11, 12, 14, 15, 3, 4, 5, 6, 7};
	int             i,
	                irq;
	SillyG_t       *sg = SillyG_;
	for (i = 0; i < sizeof(prio_table); i++) {
	    irq = prio_table[i];
	    if (config.sillyint & (1 << irq)) {
		int ret;
		enter_priv_on();
		ret = vm86_plus(VM86_REQUEST_IRQ, (SIGIO << 8) | irq);
		leave_priv_setting();
		if ( ret > 0) {
		    g_printf("Gonna monitor the IRQ %d you requested\n", irq);
		    sg->fd = -1;
		    sg->irq = irq;
		    g_printf("SIG: IRQ%d\n", irq);
		    sg++;
		}
	    }
	}
	sg->fd = 0;
	if (sg != SillyG_)
	    SillyG = SillyG_;
    }
#endif
}

void SIG_close(void)
{
#if defined(SIG)
    if (SillyG) {
	SillyG_t       *sg = SillyG;
	while (sg->fd) {
	    vm86_plus(VM86_FREE_IRQ, sg->irq);
	    sg++;
	}
	g_printf("Closing all IRQ you opened!\n");
    }
#endif
}

void sig_ctx_prepare(int tid, void *arg, void *arg2)
{
  if(in_dpmi_pm())
    fake_pm_int();
  rm_stack_enter();
  clear_IF();
}

void sig_ctx_restore(int tid, void *arg, void *arg2)
{
  rm_stack_leave();
}

static void signal_thr_post(int tid, void *arg, void *arg2)
{
  if (!in_handle_signals) {
    dbug_printf("in_handle_signals=0\n");
    return;
  }
  in_handle_signals--;
}

static void signal_thr(void *arg)
{
  struct SIGNAL_queue *sig = &signal_queue[SIGNAL_head];
  struct SIGNAL_queue sig_c;	// local copy for signal-safety
  sig_c.signal_handler = signal_queue[SIGNAL_head].signal_handler;
  sig_c.arg_size = sig->arg_size;
  if (sig->arg_size)
    memcpy(sig_c.arg, sig->arg, sig->arg_size);
  sig_c.name = sig->name;
  SIGNAL_head = (SIGNAL_head + 1) % MAX_SIG_QUEUE_SIZE;
  if (debug_level('g') > 5)
    g_printf("Processing signal %s\n", sig_c.name);
  sig_c.signal_handler(sig_c.arg);
}

/* DANG_BEGIN_FUNCTION signal_pre_init
 *
 * description:
 *  Initialize the signals to have NONE being blocked.
 * Currently this is NOT of much use to DOSEMU.
 *
 * DANG_END_FUNCTION
 *
 */
void
signal_pre_init(void)
{
  /* initialize user data & code selector values (used by DPMI code) */
  /* And save %fs, %gs for NPTL */
  eflags_fs_gs.fs = getsegment(fs);
  eflags_fs_gs.gs = getsegment(gs);
  eflags_fs_gs.eflags = getflags();
  dbug_printf("initial register values: fs: 0x%04x  gs: 0x%04x eflags: 0x%04lx\n",
    eflags_fs_gs.fs, eflags_fs_gs.gs, eflags_fs_gs.eflags);
#ifdef __x86_64__
  eflags_fs_gs.ds = getsegment(ds);
  eflags_fs_gs.es = getsegment(es);
  eflags_fs_gs.ss = getsegment(ss);
  /* get long fs and gs bases. If they are in the first 32 bits
     normal 386-style fs/gs switching can happen so we can ignore
     fsbase/gsbase */
  dosemu_arch_prctl(ARCH_GET_FS, &eflags_fs_gs.fsbase);
  if (((unsigned long)eflags_fs_gs.fsbase <= 0xffffffff) && eflags_fs_gs.fs)
    eflags_fs_gs.fsbase = 0;
  dosemu_arch_prctl(ARCH_GET_GS, &eflags_fs_gs.gsbase);
  if (((unsigned long)eflags_fs_gs.gsbase <= 0xffffffff) && eflags_fs_gs.gs)
    eflags_fs_gs.gsbase = 0;
  dbug_printf("initial segment bases: fs: %p  gs: %p\n",
    eflags_fs_gs.fsbase, eflags_fs_gs.gsbase);
#endif

  /* first set up the blocking mask: registersig() and newsetqsig()
   * adds to it */
  sigemptyset(&q_mask);
  sigemptyset(&nonfatal_q_mask);
  registersig_std(SIGALRM, SIGALRM_call);
  registersig_std(SIGIO, SIGIO_call);
  registersig_std(SIG_THREAD_NOTIFY, async_call);
  registersig(SIGCHLD, sig_child);
  newsetqsig(SIGQUIT, leavedos_signal);
  newsetqsig(SIGINT, leavedos_signal);   /* for "graceful" shutdown for ^C too*/
//  newsetqsig(SIGHUP, leavedos_emerg);
//  newsetqsig(SIGTERM, leavedos_emerg);
  /* below ones are initialized by other subsystems */
  setup_nf_sig(SIG_ACQUIRE);
  setup_nf_sig(SIG_RELEASE);
  setup_nf_sig(SIGWINCH);
  /* call that after all non-fatal sigs set up */
#ifdef __i386__
  newsetsig(SIGILL, minfault);
  newsetsig(SIGTRAP, minfault);
  newsetsig(SIGBUS, minfault);
#else
  newsetsig(SIGILL, abort_signal);
  newsetsig(SIGTRAP, abort_signal);
  newsetsig(SIGBUS, abort_signal);
#endif
  newsetsig(SIGFPE, minfault);
  newsetsig(SIGSEGV, minfault);
  newsetsig(SIGABRT, abort_signal);

  /* block async signals so that threads inherit the blockage */
  sigprocmask(SIG_BLOCK, &q_mask, NULL);

  signal(SIGPIPE, SIG_IGN);
#ifdef __linux__
  prctl(PR_SET_PDEATHSIG, SIGQUIT);
#endif

  dosemu_pthread_self = pthread_self();
  rng_init(&cbks, MAX_CBKS, sizeof(struct callback_s));

  signative_pre_init();
}

void
signal_init(void)
{
  /* signal_init is called after dpmi_setup so this check is safe */
  if (config.cpu_vm_dpmi == CPUVM_NATIVE)
    signative_init();

  sh_tid = coopth_create("signal handling", signal_thr);
  /* normally we don't need ctx handlers because the thread is detached.
   * But some crazy code (vbe.c) can call coopth_attach() on it, so we
   * set up the handlers just in case. */
  coopth_set_ctx_handlers(sh_tid, sig_ctx_prepare, sig_ctx_restore, NULL);
  coopth_set_sleep_handlers(sh_tid, handle_signals_force_enter,
	handle_signals_force_leave);
  coopth_set_permanent_post_handler(sh_tid, signal_thr_post);
  coopth_set_detached(sh_tid);

  /* mask is set up, now start using it */
  qsig_init();
  /* unblock signals in main thread */
  pthread_sigmask(SIG_UNBLOCK, &q_mask, NULL);

  sig_inited = 1;
}

void signal_done(void)
{
    struct itimerval itv;
    int i;

    itv.it_interval.tv_sec = itv.it_interval.tv_usec = 0;
    itv.it_value = itv.it_interval;
    if (setitimer(ITIMER_REAL, &itv, NULL) == -1)
	g_printf("can't turn off timer at shutdown: %s\n", strerror(errno));
    if (setitimer(ITIMER_VIRTUAL, &itv, NULL) == -1)
	g_printf("can't turn off vtimer at shutdown: %s\n", strerror(errno));
    sigprocmask(SIG_BLOCK, &nonfatal_q_mask, NULL);
    for (i = 0; i < NSIG; i++) {
	if (sigismember(&q_mask, i))
	    signal(i, SIG_DFL);
    }
    SIGNAL_head = SIGNAL_tail;
    sig_inited = 0;
}

static void handle_signals_force_enter(int tid, int sl_state)
{
  if (!in_handle_signals) {
    dosemu_error("in_handle_signals=0\n");
    return;
  }
  in_handle_signals--;
}

static void handle_signals_force_leave(int tid, void *arg, void *arg2)
{
  in_handle_signals++;
}

int signal_pending(void)
{
  return (SIGNAL_head != SIGNAL_tail);
}

/*
 * DANG_BEGIN_FUNCTION handle_signals
 *
 * description:
 *  Due to signals happening at any time, the actual work to be done
 * because a signal occurs is done here in a serial fashion.
 *
 * The concept, should this eventually work, is that a signal should only
 * flag that it has occurred and let DOSEMU deal with it in an orderly
 * fashion as it executes the rest of it's code.
 *
 * DANG_END_FUNCTION
 *
 */
void handle_signals(void)
{
  process_callbacks();
  while (signal_pending() && !in_handle_signals) {
    in_handle_signals++;
    coopth_start(sh_tid, NULL);
    coopth_run_tid(sh_tid);
  }
}

/* ==============================================================
 *
 * This is called by default at around 100Hz.
 * (see timer_interrupt_init() in init.c)
 *
 * The actual formulas, starting with the configurable parameter
 * config.freq, are:
 *	config.freq				default=18
 *	config.update = 1E6/config.freq		default=54945
 *	timer tick(us) = config.update/6	default=9157.5us
 *		       = 166667/config.freq
 *	timer tick(Hz) = 6*config.freq		default=100Hz
 *
 * 6 is the magical TIMER_DIVISOR macro used to get 100Hz
 *
 * This call should NOT be used if you need timing accuracy - many
 * signals can get lost e.g. when kernel accesses disk, and the whole
 * idea of timing-by-counting is plain wrong. We'll need the Pentium
 * counter here.
 * ============================================================== */

static void SIGALRM_call(void *arg)
{
  static int first = 0;
  static hitimer_t cnt200 = 0;
  static hitimer_t cnt1000 = 0;
  static hitimer_t cnt10 = 0;
  int i;

  if (first==0) {
    cnt200 =
    cnt1000 =
    cnt10 =
    pic_sys_time;	/* initialize */
    first = 1;
  }

  uncache_time();
  timer_tick();

  if ((pic_sys_time-cnt10) >= (PIT_TICK_RATE/100) || dosemu_frozen) {
    cnt10 = pic_sys_time;
    if (video_initialized && !config.vga)
      update_screen();
  }

  for (i = 0; i < alrm_hndl_num; i++)
    alrm_hndl[i].handler();

  alarm_idle();

  /* Here we 'type in' prestrokes from commandline, as long as there are any
   * Were won't overkill dosemu, hence we type at a speed of 14cps
   */
  if (config.pre_stroke) {
    static int count=-1;
    if (--count < 0) {
      count = type_in_pre_strokes();
      if (count <0) count =7; /* with HZ=100 we have a stroke rate of 14cps */
    }
  }

  /* this should be for per-second activities, it is actually at
   * 200ms more or less (PARTIALS=5) */
  if ((pic_sys_time-cnt200) >= (PIT_TICK_RATE/PARTIALS)) {
    cnt200 = pic_sys_time;
/*    g_printf("**** ALRM: %dms\n",(1000/PARTIALS)); */

    printer_tick(0);
    floppy_tick();
  }

/* We update the RTC from here if it has not been defined as a thread */

  /* this is for EXACT per-second activities (can produce bursts) */
  if ((pic_sys_time-cnt1000) >= PIT_TICK_RATE) {
    cnt1000 += PIT_TICK_RATE;
/*    g_printf("**** ALRM: 1sec\n"); */
    rtc_update();
  }
}

/* DANG_BEGIN_FUNCTION SIGNAL_save
 *
 * arguments:
 * context     - signal context to save.
 * signal_call - signal handling routine to be called.
 *
 * description:
 *  Save into an array structure queue the signal context of the current
 * signal as well as the function to call for dealing with this signal.
 * This is a queue because any signal may occur multiple times before
 * DOSEMU deals with it down the road.
 *
 * DANG_END_FUNCTION
 *
 */
void SIGNAL_save(void (*signal_call)(void *), void *arg, size_t len,
	const char *name)
{
  signal_queue[SIGNAL_tail].signal_handler = signal_call;
  signal_queue[SIGNAL_tail].arg_size = len;
  assert(len <= MAX_SIG_DATA_SIZE);
  if (len)
    memcpy(signal_queue[SIGNAL_tail].arg, arg, len);
  signal_queue[SIGNAL_tail].name = name;
  SIGNAL_tail = (SIGNAL_tail + 1) % MAX_SIG_QUEUE_SIZE;
}


static void SIGIO_call(void *arg){
  /* Call select to see if any I/O is ready on devices */
  irq_select();
}

__attribute__((noinline))
static void sigasync0(int sig, sigcontext_t *scp, siginfo_t *si)
{
  pthread_t tid = pthread_self();
  if (!pthread_equal(tid, dosemu_pthread_self)) {
#if defined(HAVE_PTHREAD_GETNAME_NP) && defined(__GLIBC__)
    char name[128];
    pthread_getname_np(tid, name, sizeof(name));
    dosemu_error("Async signal %i from thread %s\n", sig, name);
#else
    dosemu_error("Async signal %i from thread\n", sig);
#endif
  }
  if (sighandlers[sig])
	  sighandlers[sig](scp, si);
}

__attribute__((noinline))
static void sigasync0_std(int sig, sigcontext_t *scp, siginfo_t *si)
{
  pthread_t tid = pthread_self();
  if (!pthread_equal(tid, dosemu_pthread_self)) {
#if defined(HAVE_PTHREAD_GETNAME_NP) && defined(__GLIBC__)
    char name[128];
    pthread_getname_np(tid, name, sizeof(name));
    dosemu_error("Async signal %i from thread %s\n", sig, name);
#else
    dosemu_error("Async signal %i from thread\n", sig);
#endif
  }

  if (!asighandlers[sig]) {
    error("handler for sig %i not registered\n", sig);
    return;
  }
  SIGNAL_save(asighandlers[sig], NULL, 0, __func__);
  sigbreak(scp);
}

SIG_PROTO_PFX
static void sigasync(int sig, siginfo_t *si, void *uc)
{
  ucontext_t *uct = uc;
  sigcontext_t *scp = &uct->uc_mcontext;
  init_handler(scp, uct->uc_flags);
  sigasync0(sig, scp, si);
  deinit_handler(scp, &uct->uc_flags);
}

SIG_PROTO_PFX
static void sigasync_std(int sig, siginfo_t *si, void *uc)
{
  ucontext_t *uct = uc;
  sigcontext_t *scp = &uct->uc_mcontext;
  init_handler(scp, uct->uc_flags);
  sigasync0_std(sig, scp, si);
  deinit_handler(scp, &uct->uc_flags);
}


void do_periodic_stuff(void)
{
    check_leavedos();
    handle_signals();
#ifdef USE_MHPDBG
    /* mhp_debug() must be called exactly after handle_signals()
     * and before coopth_run(). handle_signals() feeds input to
     * debugger, and coopth_run() runs DPMI (oops!). We need to
     * run debugger before DPMI to not lose single-steps. */
    if (mhpdbg.active)
	mhp_debug(DBG_POLL, 0, 0);
#endif
    coopth_run();

    if (video_initialized && Video && Video->change_config)
	update_xtitle();
}

void add_thread_callback(void (*cb)(void *), void *arg, const char *name)
{
  if (cb) {
    struct callback_s cbk;
    int i;
    cbk.func = cb;
    cbk.arg = arg;
    cbk.name = name;
    pthread_mutex_lock(&cbk_mtx);
    i = rng_put(&cbks, &cbk);
    g_printf("callback %s added, %i queued\n", name, rng_count(&cbks));
    pthread_mutex_unlock(&cbk_mtx);
    if (!i)
      error("callback queue overflow, %s\n", name);
  }
  /* FIXME: remove the hack below */
  if (config.cpu_vm == CPUVM_EMU && config.cpu_vm_dpmi == CPUVM_EMU)
    e_gen_sigalrm();
  else
    pthread_kill(dosemu_pthread_self, SIG_THREAD_NOTIFY);
}

static void process_callbacks(void)
{
  struct callback_s cbk;
  int i;
  g_printf("processing async callbacks\n");
  do {
    pthread_mutex_lock(&cbk_mtx);
    i = rng_get(&cbks, &cbk);
    pthread_mutex_unlock(&cbk_mtx);
    if (i)
      cbk.func(cbk.arg);
  } while (i);
}

static void async_call(void *arg)
{
  process_callbacks();
}


void signal_unblock_async_sigs(void)
{
  /* unblock only nonfatal, fatals should already be unblocked */
  sigprocmask(SIG_UNBLOCK, &nonfatal_q_mask, NULL);
}

void signal_restore_async_sigs(void)
{
  /* restore temporarily unblocked async signals in a sig context.
   * Just block them even for !sas_wa because if deinit_handler is
   * interrupted after changing %fs, we are in troubles */
  sigprocmask(SIG_BLOCK, &nonfatal_q_mask, NULL);
}

void signal_unblock_fatal_sigs(void)
{
  /* unblock only nonfatal, fatals should already be unblocked */
  sigprocmask(SIG_UNBLOCK, &fatal_q_mask, NULL);
}

/* similar to above, but to call from non-signal context */
void signal_block_async_nosig(sigset_t *old_mask)
{
  assert(fault_cnt == 0);
  pthread_sigmask(SIG_BLOCK, &nonfatal_q_mask, old_mask);
}
