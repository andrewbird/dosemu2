#include "mapping.h"
#include "debug.h"

/* Define if we want graphics in X (of course we want :-) (root@zaphod) */
/* WARNING: This may not work in BSD, because it was written for Linux! */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#ifdef __linux__
#include <linux/version.h>
#endif

#include "emu.h"
#include "utilities.h"
#include "int.h"

#include "video.h"
#include "vgaemu.h" /* root@zaphod */

#include "emudpmi.h"
#include "dnative.h"
#include "cpu-emu.h"
#include "dosemu_config.h"
#include "sig.h"

/* sigaltstack work-around */
#ifdef HAVE_LINUX_SIGNAL_H
#include <linux/signal.h>
#endif
#define SIGALTSTACK_WA_DEFAULT DNATIVE
#if SIGALTSTACK_WA_DEFAULT
  #ifdef DISABLE_SYSTEM_WA
    #ifdef SS_AUTODISARM
      #define SIGALTSTACK_WA 0
    #else
      #ifdef WARN_UNDISABLED_WA
        #warning Not disabling SIGALTSTACK_WA, update your kernel
      #endif
      #define SIGALTSTACK_WA 1
    #endif
  #else
    /* work-around sigaltstack badness - disable when kernel is fixed */
    #define SIGALTSTACK_WA 1
  #endif
  #if defined(WARN_OUTDATED_WA) && defined(SS_AUTODISARM)
    #warning SIGALTSTACK_WA is outdated
  #endif
#else
  #define SIGALTSTACK_WA 0
#endif
#if SIGALTSTACK_WA
#include "mcontext.h"
#include "mapping.h"
#endif
/* SS_AUTODISARM is a dosemu-specific sigaltstack extension supported
 * by some kernels */
#ifndef SS_AUTODISARM
#define SS_AUTODISARM  (1U << 31)    /* disable sas during sighandling */
#endif

/* sigreturn work-around */
#ifdef HAVE_ASM_UCONTEXT_H
#include <asm/ucontext.h>
#endif
#ifdef __x86_64__
  #define SIGRETURN_WA_DEFAULT DNATIVE
#else
  #define SIGRETURN_WA_DEFAULT 0
#endif
#if SIGRETURN_WA_DEFAULT
  #ifdef DISABLE_SYSTEM_WA
    #ifdef UC_SIGCONTEXT_SS
      #define SIGRETURN_WA 0
    #else
      #ifdef WARN_UNDISABLED_WA
        #warning Not disabling SIGRETURN_WA, update your kernel
      #endif
      #define SIGRETURN_WA 1
    #endif
  #else
    /* work-around sigreturn badness - disable when kernel is fixed */
    #define SIGRETURN_WA 1
  #endif
  #if defined(WARN_OUTDATED_WA) && defined(UC_SIGCONTEXT_SS)
    #warning SIGRETURN_WA is outdated
  #endif
#else
  #define SIGRETURN_WA 0
#endif

static void *cstack;
static struct sigaction sacts[NSIG];
static int block_all_sigs;
#if SIGALTSTACK_WA
static void *backup_stack;
static int need_sas_wa;
#endif
#if SIGRETURN_WA
static int need_sr_wa;
#endif

#if SIGRETURN_WA
static unsigned int *iret_frame;

asm("\n\
	.globl DPMI_iret\n\
	.type DPMI_iret,@function\n\
DPMI_iret:\n\
	iretl\n\
");
extern void DPMI_iret(void);

static void iret_frame_alloc(void)
{
    unsigned int i, j;
    void *addr;
    /* search for page with bits 16-31 clear within first 47 bits
       of address space */
    for (i = 1; i < 0x8000; i++) {
        for (j = 0; j < 0x10000; j += PAGE_SIZE) {
            addr = (void *) (i * 0x100000000UL + j);
            iret_frame =
                mmap_mapping(MAPPING_SCRATCH | MAPPING_NOOVERLAP,
                             addr, PAGE_SIZE, PROT_READ | PROT_WRITE);
            if (iret_frame != MAP_FAILED)
                goto out;
        }
    }
out:
    if (iret_frame != addr) {
        error("Can't find DPMI iret page, leaving\n");
        config.exitearly = 1;
    }
}

static void iret_frame_setup(sigcontext_t * scp)
{
    /* set up a frame to get back to DPMI via iret. The kernel does not save
       %ss, and the SYSCALL instruction in sigreturn() destroys it.

       IRET pops off everything in 64-bit mode even if the privilege
       does not change which is nice, but clobbers the high 48 bits
       of rsp if the DPMI client uses a 16-bit stack which is not so
       nice (see EMUfailure.txt). Setting %rsp to 0x100000000 so that
       bits 16-31 are zero works around this problem, as DPMI code
       can't see bits 32-63 anyway.
     */

    iret_frame[0] = _scp_eip;
    iret_frame[1] = _scp_cs;
    iret_frame[2] = _scp_eflags;
    iret_frame[3] = _scp_esp;
    iret_frame[4] = _scp_ss;
    _scp_rsp = (unsigned long) iret_frame;
}

static void dpmi_iret_setup(sigcontext_t * scp)
{
    iret_frame_setup(scp);
    _scp_eflags &= ~TF;
    _scp_rip = (unsigned long) DPMI_iret;
    _scp_cs = getsegment(cs);
}

static void dpmi_iret_unwind(sigcontext_t * scp)
{
    if (_scp_rip != (unsigned long) DPMI_iret)
        return;
    _scp_eip = iret_frame[0];
    _scp_cs = iret_frame[1];
    _scp_eflags = iret_frame[2];
    _scp_esp = iret_frame[3];
    _scp_ss = iret_frame[4];
}
#endif

#ifdef __x86_64__
#ifndef UC_SIGCONTEXT_SS
/*
 * UC_SIGCONTEXT_SS will be set when delivering 64-bit or x32 signals on
 * kernels that save SS in the sigcontext.  Kernels that set UC_SIGCONTEXT_SS
 * allow signal handlers to set UC_STRICT_RESTORE_SS;
 * if UC_STRICT_RESTORE_SS is set, then sigreturn will restore SS.
 *
 * For compatibility with old programs, the kernel will *not* set
 * UC_STRICT_RESTORE_SS when delivering signals from 32bit code.
 */
#define UC_SIGCONTEXT_SS       0x2
#define UC_STRICT_RESTORE_SS   0x4
#endif
#endif

static void print_exception_info(sigcontext_t *scp);

static int dpmi_fault(sigcontext_t *scp)
{
  /* If this is an exception 0x11, we have to ignore it. The reason is that
   * under real DOS the AM bit of CR0 is not set.
   * Also clear the AC flag to prevent it from re-occuring.
   */
  if (_scp_trapno == 0x11) {
    g_printf("Exception 0x11 occurred, clearing AC\n");
    _scp_eflags &= ~AC;
    return DPMI_RET_CLIENT;
  }

  return DPMI_RET_FAULT;	// process the rest in dosemu context
}

/*
 * DANG_BEGIN_FUNCTION dosemu_fault(int, sigcontext_t);
 *
 * All CPU exceptions (except 13=general_protection from V86 mode,
 * which is directly scanned by the kernel) are handled here.
 *
 * We have 4 main cases:
 * 1. VM86 faults from vm86() (i386 only)
 * 2. DPMI faults with LDT _cs (native DPMI only)
 * 3. Faults (PF/DE) generated from cpuemu. In this case _cs is the Linux
 *    userspace _cs
 * 4. DOSEMU itself crashes (bad!)
 *
 * DANG_END_FUNCTION
 */
static void dosemu_fault1(int signum, sigcontext_t *scp)
{
  if (fault_cnt > 1) {
    error("Fault handler re-entered! signal=%i _trapno=0x%x\n",
      signum, _scp_trapno);
    if (!in_vm86 && !DPMIValidSelector(_scp_cs)) {
      gdb_debug();
      _exit(43);
    } else {
      error("BUG: Fault handler re-entered not within dosemu code! in_vm86=%i\n",
        in_vm86);
    }
    goto bad;
  }
#ifdef __x86_64__
  if (_scp_trapno == 0x0e && _scp_cr2 > 0xffffffff)
  {
#ifdef X86_EMULATOR
    if (IS_EMU() && !CONFIG_CPUSIM && e_in_compiled_code()) {
      int i;
      /* dosemu_error() will SIGSEGV in backtrace(). */
      error("JIT fault accessing invalid address 0x%08"PRI_RG", "
          "RIP=0x%08"PRI_RG"\n", _scp_cr2, _scp_rip);
      if (mapping_find_hole(_scp_rip, _scp_rip + 64, 1) == MAP_FAILED) {
        error("@Generated code dump:\n");
        for (i = 0; i < 64; i++) {
          error("@ %02x", *(unsigned char *)(_scp_rip + i));
          if ((i & 15) == 15)
            error("@\n");
        }
      }
      goto bad;
    }
#endif
    dosemu_error("Accessing invalid address 0x%08"PRI_RG"\n", _scp_cr2);
    goto bad;
  }
#endif


#ifdef __i386__
  /* case 1: note that _scp_cr2 must be 0-based */
  if (in_vm86 && config.cpu_vm == CPUVM_VM86) {
    true_vm86_fault(scp);
    return;
  }
#endif

  /* case 2: At first let's find out where we came from */
  if (DPMIValidSelector(_scp_cs)) {
    int ret = DPMI_RET_FAULT;
    assert(config.cpu_vm_dpmi == CPUVM_NATIVE);
    if (_scp_trapno == 0x10) {
      dbug_printf("coprocessor exception, calling IRQ13\n");
      print_exception_info(scp);
      pic_untrigger(13);
      pic_request(13);
      dpmi_return(scp, DPMI_RET_DOSEMU);
      return;
    }

    /* Not in dosemu code: dpmi_fault() will handle that */
    if (ret == DPMI_RET_FAULT)
      ret = dpmi_fault(scp);
    if (signal_pending() || ret != DPMI_RET_CLIENT)
      dpmi_return(scp, ret);
    return;
  }

#ifdef X86_EMULATOR
  /* case 3 */
  if (IS_EMU() && e_emu_fault(scp, in_vm86))
    return;
#endif

  /* case 4 */
  error("Fault in dosemu code, in_dpmi=%i\n", dpmi_active());
  /* TODO - we can start gdb here */
  /* start_gdb() */
  /* Going to die from here */

bad:
/* All recovery attempts failed, going to die :( */

  {
#ifdef __x86_64__
    unsigned char *fsbase, *gsbase;
#endif
    error("cpu exception in dosemu code outside of %s!\n"
	  "sig: %i trapno: 0x%02x  errorcode: 0x%08x  cr2: 0x%08"PRI_RG"\n"
	  "eip: 0x%08"PRI_RG"  esp: 0x%08"PRI_RG"  eflags: 0x%08x\n"
	  "cs: 0x%04x  ds: 0x%04x  es: 0x%04x  ss: 0x%04x\n"
	  "fs: 0x%04x  gs: 0x%04x\n",
	  (in_dpmi_pm() ? "DPMI client" : "VM86()"),
	  signum, _scp_trapno, _scp_err, _scp_cr2,
	  _scp_rip, _scp_rsp, _scp_eflags, _scp_cs, _scp_ds, _scp_es, _scp_ss, _scp_fs, _scp_gs);
#ifdef __x86_64__
    dosemu_arch_prctl(ARCH_GET_FS, &fsbase);
    dosemu_arch_prctl(ARCH_GET_GS, &gsbase);
    error("@fsbase: %p gsbase: %p\n", fsbase, gsbase);
#endif
    error("@\n");

    error("Please install gdb, update dosemu from git, compile it with debug\n"
        "info and make a bug report with the content of ~/.dosemu/boot.log at\n"
"https://github.com/dosemu2/dosemu2/issues\n");
    error("@Please provide any additional info you can, like the test-cases,\n"
          "URLs and all the rest that fits.\n\n");
#ifdef X86_EMULATOR
    /* gdb_debug() will crash in jit code doing backtrace() */
    if (!(IS_EMU() && !CONFIG_CPUSIM && e_in_compiled_code()))
#endif
    gdb_debug();

    if (DPMIValidSelector(_scp_cs))
      print_exception_info(scp);
    if (in_vm86)
	show_regs();
    fatalerr = 4;
    _leavedos_main(0, signum);		/* shouldn't return */
  }
}

/* noinline is to prevent gcc from moving TLS access around init_handler() */
__attribute__((noinline))
static void dosemu_fault0(int signum, sigcontext_t *scp)
{
  pthread_t tid;

  if (fault_cnt > 2) {
   /*
    * At this point we already tried leavedos(). Now try _exit()
    * and NOT exit(3), because glibc is probably malfunctions if
    * we are here.
    */
    _exit(255);
  }

  tid = pthread_self();
  if (!pthread_equal(tid, dosemu_pthread_self)) {
#ifdef __GLIBC__
    char name[128];
#endif
    /* disable cancellation to prevent main thread from terminating
     * this one due to SIGSEGV elsewhere while we are doing backtrace */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
#if defined(HAVE_PTHREAD_GETNAME_NP) && defined(__GLIBC__)
    pthread_getname_np(tid, name, sizeof(name));
    dosemu_error("thread %s got signal %i, cr2=%llx\n", name, signum,
	(unsigned long long)_scp_cr2);
#else
    dosemu_error("thread got signal %i, cr2=%llx\n", signum,
	(unsigned long long)_scp_cr2);
#endif
    signal(signum, SIG_DFL);
    pthread_kill(tid, signum);  // dump core
    _exit(23);
    return;
  }

#ifdef __linux__
  if (kernel_version_code < KERNEL_VERSION(2, 6, 14)) {
    sigset_t set;

    /* this emulates SA_NODEFER, so that we can double fault.
       SA_NODEFER only works as documented in Linux kernels >= 2.6.14.
    */
    sigemptyset(&set);
    sigaddset(&set, signum);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
  }
#endif

  if (debug_level('g')>7)
    g_printf("Entering fault handler, signal=%i _trapno=0x%x\n",
      signum, _scp_trapno);

  dosemu_fault1(signum, scp);

  if (debug_level('g')>8)
    g_printf("Returning from the fault handler\n");
}

SIG_PROTO_PFX
static void dosemu_fault(int signum, siginfo_t *si, void *uc)
{
  ucontext_t *uct = uc;
  sigcontext_t *scp = &uct->uc_mcontext;
  /* need to call init_handler() before any syscall.
   * Additionally, TLS access should be done in a separate no-inline
   * function, so that gcc not to move the TLS access around init_handler(). */
  init_handler(scp, uct->uc_flags);
#if defined(__FreeBSD__)
  /* freebsd does not provide cr2 */
  _scp_cr2 = (uintptr_t)si->si_addr;
#endif
  fault_cnt++;
  dosemu_fault0(signum, scp);
  fault_cnt--;
  deinit_handler(scp, &uct->uc_flags);
}

/*
 * DANG_BEGIN_FUNCTION print_exception_info
 *
 * Prints information about an exception: exception number, error code,
 * address, reason, etc.
 *
 * DANG_END_FUNCTION
 *
 */
static void print_exception_info(sigcontext_t *scp)
{
  int i;

  switch(_scp_trapno)
    {
    case 0:
      error("@Division by zero\n");
      break;


    case 1:
      error("@Debug exception\n");
      break;


    case 3:
      error("@Breakpoint exception (caused by INT 3 instruction)\n");
      break;


    case 4:
      error("@Overflow exception (caused by INTO instruction)\n");
      break;


    case 5:
      error("@Bound exception (caused by BOUND instruction)\n");
      break;


    case 6: {
      unsigned char *csp;
      int ps = getpagesize();
      unsigned pa = _scp_rip & (ps - 1);
      int sub = _min(pa, 10);
      int sup = _min(ps - pa, 10);
      error("@Invalid opcode\n");
      error("@Opcodes: ");
      csp = (unsigned char *) _scp_rip - sub;
      for (i = 0; i < 10 - sub; i++)
        error("@XX ");
      for (i = 0; i < sub; i++)
	error("@%02x ", *csp++);
      error("@-> ");
      for (i = 0; i < sup; i++)
	error("@%02x ", *csp++);
      for (i = 0; i < 10 - sup; i++)
        error("@XX ");
      error("@\n");
      break;
    }

    case 7:
      error("@Coprocessor exception (coprocessor not available)\n");
      /* I'd like to print some info on the EM, MP, and TS flags in CR0,
       * but I don't know where I can get that information :-(
       * Anyway, this exception should not happen... (Erik Mouw)
       */
      break;


    case 8:
      error("@Double fault\n");
      break;


    case 9:
      error("@Coprocessor segment overflow\n");
      break;


    case 0xa:
      error("@Invalid TSS\n");
      if(_scp_err & 0x02)
	error("@IDT");
      else if(_scp_err & 0x04)
	error("@LDT");
      else
	error("@GDT");

      error("@ selector: 0x%04x\n", (unsigned)((_scp_err >> 3) & 0x1fff ));

      if(_scp_err & 0x01)
	error("@Exception was not caused by DOSEMU\n");
      else
	error("@Exception was caused by DOSEMU\n");
      break;


    case 0xb:
      error("@Segment not available\n");
      /* This is the same code as case 0x0a; the compiler merges these
       * blocks, so I don't have to edit some dirty constructions to
       * generate one block of code. (Erik Mouw)
       */
      if(_scp_err & 0x02)
	error("@IDT");
      else if(_scp_err & 0x04)
	error("@LDT");
      else
	error("@GDT");

      error("@ selector: 0x%04x\n", (unsigned)((_scp_err >> 3) & 0x1fff ));

      if(_scp_err & 0x01)
	error("@Exception was not caused by DOSEMU\n");
      else
	error("@Exception was caused by DOSEMU\n");
      break;


    case 0xc:
      error("@Stack exception\n");
      break;


    case 0xd:
      error("@General protection exception\n");
      /* This is the same code as case 0x0a; the compiler merges these
       * blocks, so I don't have to edit some dirty constructions to
       * generate one block of code. (Erik Mouw)
       */
      if(_scp_err & 0x02)
	error("@IDT");
      else if(_scp_err & 0x04)
	error("@LDT");
      else
	error("@GDT");

      error("@ selector: 0x%04x\n", (unsigned)((_scp_err >> 3) & 0x1fff ));

      if(_scp_err & 0x01)
	error("@Exception was not caused by DOSEMU\n");
      else
	error("@Exception was caused by DOSEMU\n");
      break;


    case 0xe:
      error("@Page fault: ");
      if(_scp_err & 0x02)
	error("@write");
      else
	error("@read");

      error("@ instruction to linear address: 0x%08"PRI_RG"\n", _scp_cr2);

      error("@CPU was in ");
      if(_scp_err & 0x04)
	error("@user mode\n");
      else
	error("@supervisor mode\n");

      error("@Exception was caused by ");
      if(_scp_err & 0x01)
	error("@insufficient privilege\n");
      else
	error("@non-available page\n");
      break;

   case 0x10: {
      int i, n;
      unsigned short sw;
      fpregset_t p = _scp_fpstate;
      error ("@Coprocessor Error:\n");
#ifdef __x86_64__
      error ("@cwd=%04x swd=%04x ftw=%04x\n", p->cwd, p->swd, p->ftw);
      error ("@cs:rip=%04x:%08lx ds:data=%04x:%08lx\n",	_scp_cs,p->rip,_scp_ds,p->rdp);
      sw = p->swd;
#else
      error ("@cw=%04x sw=%04x tag=%04x\n",
	     ((unsigned short)(p->cw)),((unsigned short)(p->sw)),
	((unsigned short)(p->tag)));
      error ("@cs:eip=%04x:%08x ds:data=%04x:%08x\n",
	     ((unsigned short)(p->cssel)),(unsigned)p->ipoff,
	     ((unsigned short)(p->datasel)),(unsigned)p->dataoff);
      sw = p->sw;
#endif
      if ((sw&0x80)==0) error("@No error summary bit,why?\n");
      else {
	if (sw&0x20) error("@Precision\n");
	if (sw&0x10) error("@Underflow\n");
	if (sw&0x08) error("@Overflow\n");
	if (sw&0x04) error("@Divide by 0\n");
	if (sw&0x02) error("@Denormalized\n");
	if ((sw&0x41)==0x01) error("@Invalid op\n");
	  else if ((sw&0x41)==0x41) error("@Stack fault\n");
      }
      n = (sw >> 11) & 7;
      for (i=0; i<8; i++) {
	unsigned short *r = p->_st[i].significand;
	unsigned short e = p->_st[i].exponent;
	error ("@fpr[%d] = %04x:%04x%04x%04x%04x\n",n,e,r[3],r[2],r[1],r[0]);
	n = (n+1) & 7;
      }
      } break;

   case 0x13: {
#ifdef __x86_64__
      int i;
      unsigned mxcsr;
      fpregset_t p = _scp_fpstate;
      error ("@SIMD Floating-Point Exception:\n");
      mxcsr = p->mxcsr;
      error ("@mxcsr=%08x, mxcr_mask=%08x\n",mxcsr,(unsigned)(p->mxcr_mask));
      if (mxcsr&0x40) error("@Denormals are zero\n");
      if (mxcsr&0x20) error("@Precision\n");
      if (mxcsr&0x10) error("@Underflow\n");
      if (mxcsr&0x08) error("@Overflow\n");
      if (mxcsr&0x04) error("@Divide by 0\n");
      if (mxcsr&0x02) error("@Denormalized\n");
      if (mxcsr&0x01) error("@Invalid op\n");
      for (i=0; i<sizeof(p->_xmm)/sizeof(p->_xmm[0]); i++)
      {
	error ("@xmm[%d] = %08x:%08x:%08x:%08x\n",i,
	      (unsigned)p->_xmm[i].element[0], (unsigned)p->_xmm[i].element[1],
	      (unsigned)p->_xmm[i].element[2], (unsigned)p->_xmm[i].element[3]);
      }
#else
      error ("@SIMD Floating-Point Exception\n");
#endif
      break;
    }

    default:
      error("@Unknown exception\n");
      break;
    }
}

static void sigstack_init(void)
{
#ifndef MAP_STACK
#define MAP_STACK 0
#endif

  /* sigaltstack_wa is optional. See if we need it. */
  /* .ss_flags is signed int and SS_AUTODISARM is a sign bit :( */
  stack_t dummy2;
  stack_t dummy = { .ss_flags = (int)(SS_DISABLE | SS_AUTODISARM) };
  int err = sigaltstack(&dummy, &dummy2);
  int errno_save = errno;

  /* needs to drop SS_AUTODISARM or asan will fail. See
   * https://github.com/dosemu2/dosemu2/issues/1576 */
  sigaltstack(&dummy2, NULL);
#if SIGALTSTACK_WA
  if ((err && errno_save == EINVAL)
#ifdef __i386__
#ifdef __linux__
      /* kernels before 4.11 had the needed functionality only for 64bits */
      || kernel_version_code < KERNEL_VERSION(4, 11, 0)
#endif
#endif
     )
  {
    need_sas_wa = 1;
    warn("Enabling sigaltstack() work-around\n");
    /* for SAS WA block all signals. If we don't, there is a
     * race that the signal can come after we switched to backup stack
     * but before we disabled sigaltstack. We unblock the fatal signals
     * later, only right before switching back to dosemu. */
    block_all_sigs = 1;
  } else if (err) {
    goto unk_err;
  }

  if (need_sas_wa) {
    cstack = alloc_mapping(MAPPING_SHARED, SIGSTACK_SIZE);
    if (cstack == MAP_FAILED) {
      error("Unable to allocate stack\n");
      config.exitearly = 1;
      return;
    }
    backup_stack = alias_mapping_ux(MAPPING_OTHER, SIGSTACK_SIZE,
	PROT_READ | PROT_WRITE, cstack);
    if (backup_stack == MAP_FAILED) {
      error("Unable to allocate stack\n");
      config.exitearly = 1;
      return;
    }
  } else {
    cstack = mmap(NULL, SIGSTACK_SIZE, PROT_READ | PROT_WRITE,
	MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (cstack == MAP_FAILED) {
      error("Unable to allocate stack\n");
      config.exitearly = 1;
      return;
    }
  }
#else
  if ((err && errno_save == EINVAL)
#ifdef __i386__
#ifdef __linux__
      || kernel_version_code < KERNEL_VERSION(4, 11, 0)
#endif
#endif
     )
  {
    error("Your kernel does not support SS_AUTODISARM and the "
	  "work-around in dosemu is not enabled.\n");
    config.exitearly = 1;
    return;
  } else if (err) {
    goto unk_err;
  }
  cstack = mmap(NULL, SIGSTACK_SIZE, PROT_READ | PROT_WRITE,
	MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (cstack == MAP_FAILED) {
    error("Unable to allocate stack\n");
    config.exitearly = 1;
    return;
  }
#endif

  return;

unk_err:
  if (err) {
    error("sigaltstack() returned %i, %s\n", errno_save,
        strerror(errno_save));
    config.exitearly = 1;
  }
}

void signative_init(void)
{
  sigstack_init();
#if SIGRETURN_WA
  /* 4.6+ are able to correctly restore SS */
#ifdef __linux__
  if (kernel_version_code < KERNEL_VERSION(4, 6, 0)) {
    need_sr_wa = 1;
    warn("Enabling sigreturn() work-around for old kernel\n");
    /* block all sigs for SR WA. If we don't, the signal can come before
     * SS is saved, but we can't restore SS on signal exit. */
    block_all_sigs = 1;
  }
#endif
#endif
}

void signal_set_altstack(int on)
{
  stack_t stk = { 0 };
  int err;

  if (!on) {
    stk.ss_flags = SS_DISABLE;
    err = sigaltstack(&stk, NULL);
  } else {
    stk.ss_sp = cstack;
    stk.ss_size = SIGSTACK_SIZE;
#if SIGALTSTACK_WA
    stk.ss_flags = SS_ONSTACK | (need_sas_wa ? 0 : SS_AUTODISARM);
#else
    stk.ss_flags = SS_ONSTACK | SS_AUTODISARM;
#endif
    err = sigaltstack(&stk, NULL);
  }
  if (err) {
    error("sigaltstack(0x%x) returned %i, %s\n",
        stk.ss_flags, err, strerror(errno));
    leavedos(err);
  }
}

#if SIGALTSTACK_WA
static void signal_sas_wa(void)
{
  int err;
  stack_t ss = {};
  m_ucontext_t hack;
  unsigned char *sp;
  unsigned char *top = cstack + SIGSTACK_SIZE;
  unsigned char *btop = backup_stack + SIGSTACK_SIZE;
  ptrdiff_t delta;

  if (getmcontext(&hack) == 0) {
    sp = alloca(sizeof(void *));
    delta = top - sp;
    /* switch stack to its mirror (same phys addr) to cheat sigaltstack() */
    asm volatile(
#ifdef __x86_64__
    "mov %0, %%rsp\n"
#else
    "mov %0, %%esp\n"
#endif
     :: "r"(btop - delta));
  } else {
    signal_unblock_fatal_sigs();
    return;
  }

  ss.ss_flags = SS_DISABLE;
  /* sas will re-enable itself when returning from sighandler */
  err = sigaltstack(&ss, NULL);
  if (err)
    perror("sigaltstack");

  setmcontext(&hack);
}
#endif

void signative_pre_init(void)
{
#if SIGRETURN_WA
  if (config.cpu_vm_dpmi == CPUVM_NATIVE)
    iret_frame_alloc();
#endif
}

SIG_PROTO_PFX
void signative_enter(sigcontext_t *scp)
{
#if SIGRETURN_WA
  if (need_sr_wa && !DPMIValidSelector(_scp_cs))
    dpmi_iret_unwind(scp);
#endif

#if 0
  /* for async signals need to restore fs/gs even if dosemu code
   * was interrupted, because it can be interrupted in a switching
   * routine when fs or gs are already switched but cs is not */
  if (!DPMIValidSelector(_scp_cs) && !async)
    return;
#else
  /* as DIRECT_DPMI_SWITCH support is now removed, the above comment
   * applies only to DPMI_iret, which is now unwound.
   * We don't need to restore segregs for async signals any more. */
  if (!DPMIValidSelector(_scp_cs))
    return;
#endif

  /* restore %fs and %gs for compatibility with NPTL. */
  if (getsegment(fs) != eflags_fs_gs.fs)
    loadregister(fs, eflags_fs_gs.fs);
  if (getsegment(gs) != eflags_fs_gs.gs)
    loadregister(gs, eflags_fs_gs.gs);
#ifdef __x86_64__
  loadregister(ds, eflags_fs_gs.ds);
  loadregister(es, eflags_fs_gs.es);
  /* kernel has the following rule: non-zero selector means 32bit base
   * in GDT. Zero selector means 64bit base, set via msr.
   * So if we set selector to 0, need to use also prctl(ARCH_SET_xS).
   * Also, if the bases are not used they are 0 so no need to restore,
   * which saves a syscall */
  if (!eflags_fs_gs.fs && eflags_fs_gs.fsbase)
    dosemu_arch_prctl(ARCH_SET_FS, eflags_fs_gs.fsbase);
  if (!eflags_fs_gs.gs && eflags_fs_gs.gsbase)
    dosemu_arch_prctl(ARCH_SET_GS, eflags_fs_gs.gsbase);
#endif
}

SIG_PROTO_PFX
void signative_leave(sigcontext_t *scp, unsigned long *uc_flags)
{
  if (!DPMIValidSelector(_scp_cs))
    return;

#ifdef __x86_64__
  if (*uc_flags & UC_SIGCONTEXT_SS) {
    /*
     * On Linux 4.4 (possibly) and up, the kernel can fully restore
     * SS and ESP, so we don't need any special tricks.  To avoid confusion,
     * force strict restore.  (Some 4.1 versions support this as well but
     * without the uc_flags bits.  It's not trying to detect those kernels.)
     */
    *uc_flags |= UC_STRICT_RESTORE_SS;
  } else {
#if SIGRETURN_WA
    if (!need_sr_wa) {
      need_sr_wa = 1;
      warn("Enabling sigreturn() work-around\n");
    }
    dpmi_iret_setup(scp);
#else
    error("Your kernel does not support UC_STRICT_RESTORE_SS and the "
	  "work-around in dosemu is not enabled.\n");
    dpmi_return(scp, DPMI_RET_EXIT);
    leavedos_sig(-1);
#endif
  }

  if (_scp_fs != getsegment(fs))
    loadregister(fs, _scp_fs);
  if (_scp_gs != getsegment(gs))
    loadregister(gs, _scp_gs);

  loadregister(ds, _scp_ds);
  loadregister(es, _scp_es);
#endif
}

int signative_skip_unblock(sigcontext_t *scp)
{
#if SIGALTSTACK_WA
  /* for SAS WA we unblock the fatal signals even later if we came
   * from DPMI, as then we'll be switching stacks which is racy when
   * async signals enabled. */
  return (need_sas_wa && DPMIValidSelector(_scp_cs));
#endif
  return 0;
}

#ifdef __x86_64__
int signative_skip_ss(unsigned long uc_flags)
{
  return (uc_flags & UC_SIGCONTEXT_SS);
}
#endif

SIG_PROTO_PFX
static void fixup_handler(int sig, siginfo_t *si, void *uc)
{
	struct sigaction *sa;
	ucontext_t *uct = uc;
	sigcontext_t *scp = &uct->uc_mcontext;
	init_handler(scp, 1);
	sa = &sacts[sig];
	if (sa->sa_flags & SA_SIGINFO) {
		sa->sa_sigaction(sig, si, uc);
	} else {
		typedef void (*hdlr_t)(int, siginfo_t *, ucontext_t *);
		hdlr_t hdlr = (hdlr_t)sa->sa_handler;
		hdlr(sig, si, uc);
	}
	deinit_handler(scp, &uct->uc_flags);
}

static void fixupsig(int sig)
{
	struct sigaction sa;
	sigaction(sig, NULL, &sa);
	sacts[sig] = sa;
	if (sa.sa_handler == SIG_DFL || sa.sa_handler == SIG_IGN)
		return;
	sa.sa_flags |= SA_ONSTACK | SA_SIGINFO;
	sa.sa_sigaction = fixup_handler;
	sigaction(sig, &sa, NULL);
}

void unsetsig(int sig)
{
	sigaction(sig, &sacts[sig], NULL);
}

static void newsetsig(int sig, void (*fun)(int sig, siginfo_t *si, void *uc))
{
	struct sigaction sa;

	sa.sa_flags = SA_RESTART | SA_ONSTACK | SA_SIGINFO;
#ifdef __linux__
	if (kernel_version_code >= KERNEL_VERSION(2, 6, 14))
#endif
		sa.sa_flags |= SA_NODEFER;
	if (block_all_sigs)
	{
		/* initially block all async signals. */
		sa.sa_mask = q_mask;
	}
	else
	{
		/* block all non-fatal async signals */
		sa.sa_mask = nonfatal_q_mask;
	}
	sa.sa_sigaction = fun;
	sigaction(sig, &sa, &sacts[sig]);
}

static int saved_fc;

void signal_switch_to_dosemu(void)
{
  saved_fc = fault_cnt;
  fault_cnt = 0;
}

void signal_switch_to_dpmi(void)
{
  fault_cnt = saved_fc;
}

void signal_return_to_dosemu(void)
{
#if SIGALTSTACK_WA
  if (need_sas_wa)
    signal_sas_wa();
#endif
}

void signal_return_to_dpmi(void)
{
}

int signative_block_all_sigs(void)
{
    return block_all_sigs;
}

void signative_start(void)
{
  fixupsig(SIGPROF);
  /* call that after all non-fatal sigs set up */
  newsetsig(SIGILL, dosemu_fault);
  newsetsig(SIGFPE, dosemu_fault);
  newsetsig(SIGTRAP, dosemu_fault);
  newsetsig(SIGBUS, dosemu_fault);
  newsetsig(SIGSEGV, dosemu_fault);
  newsetsig(DPMI_TMP_SIG, dpmi_switch_sa);  // dont unset this on stop
}

void signative_stop(void)
{
  unsetsig(SIGPROF);
  unsetsig(SIGILL);
  unsetsig(SIGFPE);
  unsetsig(SIGTRAP);
  unsetsig(SIGBUS);
  unsetsig(SIGSEGV);
}
