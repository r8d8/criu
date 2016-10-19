#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <errno.h>
#include "uapi/std/syscall-codes.h"
#include "asm/types.h"
#include "ptrace.h"
#include "parasite-syscall.h"
#include "errno.h"
#include "criu-log.h"
#include "infect.h"
#include "infect-priv.h"

#ifndef NT_PPC_TM_SPR
#define NT_PPC_TM_CGPR  0x108           /* TM checkpointed GPR Registers */
#define NT_PPC_TM_CFPR  0x109           /* TM checkpointed FPR Registers */
#define NT_PPC_TM_CVMX  0x10a           /* TM checkpointed VMX Registers */
#define NT_PPC_TM_CVSX  0x10b           /* TM checkpointed VSX Registers */
#define NT_PPC_TM_SPR   0x10c           /* TM Special Purpose Registers */
#endif

/* FIXME -- copied between crtools.c and infect.c */
#define MSR_TMA (1UL<<34)	/* bit 29 Trans Mem state: Transactional */
#define MSR_TMS (1UL<<33)	/* bit 30 Trans Mem state: Suspended */
#define MSR_TM  (1UL<<32)	/* bit 31 Trans Mem Available */
#define MSR_VEC (1UL<<25)
#define MSR_VSX (1UL<<23)

#define MSR_TM_ACTIVE(x) ((((x) & MSR_TM) && ((x)&(MSR_TMA|MSR_TMS))) != 0)

/* This is the layout of the POWER7 VSX registers and the way they
 * overlap with the existing FPR and VMX registers.
 *
 *                 VSR doubleword 0               VSR doubleword 1
 *         ----------------------------------------------------------------
 * VSR[0]  |             FPR[0]            |                              |
 *         ----------------------------------------------------------------
 * VSR[1]  |             FPR[1]            |                              |
 *         ----------------------------------------------------------------
 *         |              ...              |                              |
 *         ----------------------------------------------------------------
 * VSR[30] |             FPR[30]           |                              |
 *         ----------------------------------------------------------------
 * VSR[31] |             FPR[31]           |                              |
 *         ----------------------------------------------------------------
 * VSR[32] |                             VR[0]                            |
 *         ----------------------------------------------------------------
 * VSR[33] |                             VR[1]                            |
 *         ----------------------------------------------------------------
 *         |                              ...                             |
 *         ----------------------------------------------------------------
 * VSR[62] |                             VR[30]                           |
 *         ----------------------------------------------------------------
 * VSR[63] |                             VR[31]                           |
 *         ----------------------------------------------------------------
 *
 * PTRACE_GETFPREGS returns FPR[0..31] + FPSCR
 * PTRACE_GETVRREGS returns VR[0..31] + VSCR + VRSAVE
 * PTRACE_GETVSRREGS returns VSR[0..31]
 *
 * PTRACE_GETVSRREGS and PTRACE_GETFPREGS are required since we need
 * to save FPSCR too.
 *
 * There 32 VSX double word registers to save since the 32 first VSX double
 * word registers are saved through FPR[0..32] and the remaining registers
 * are saved when saving the Altivec registers VR[0..32].
 */

static int get_fpu_regs(pid_t pid, user_fpregs_struct_t *fp)
{
	if (ptrace(PTRACE_GETFPREGS, pid, 0, (void *)&fp->fpregs) < 0) {
		pr_perror("Couldn't get floating-point registers");
		return -1;
	}
	fp->flags |= USER_FPREGS_FL_FP;

	return 0;
}

static int get_altivec_regs(pid_t pid, user_fpregs_struct_t *fp)
{
	if (ptrace(PTRACE_GETVRREGS, pid, 0, (void*)&fp->vrregs) < 0) {
		/* PTRACE_GETVRREGS returns EIO if Altivec is not supported.
		 * This should not happen if msr_vec is set. */
		if (errno != EIO) {
			pr_perror("Couldn't get Altivec registers");
			return -1;
		}
		pr_debug("Altivec not supported\n");
	}
	else {
		pr_debug("Dumping Altivec registers\n");
		fp->flags |= USER_FPREGS_FL_ALTIVEC;
	}
	return 0;
}

/*
 * Since the FPR[0-31] is stored in the first double word of VSR[0-31] and
 * FPR are saved through the FP state, there is no need to save the upper part
 * of the first 32 VSX registers.
 * Furthermore, the 32 last VSX registers are also the 32 Altivec registers
 * already saved, so no need to save them.
 * As a consequence, only the doubleword 1 of the 32 first VSX registers have
 * to be saved (the ones are returned by PTRACE_GETVSRREGS).
 */
static int get_vsx_regs(pid_t pid, user_fpregs_struct_t *fp)
{
	if (ptrace(PTRACE_GETVSRREGS, pid, 0, (void*)fp->vsxregs) < 0) {
		/*
		 * EIO is returned in the case PTRACE_GETVRREGS is not
		 * supported.
		 */
		if (errno != EIO) {
			pr_perror("Couldn't get VSX registers");
			return -1;
		}
		pr_debug("VSX register's dump not supported.\n");
	}
	else {
		pr_debug("Dumping VSX registers\n");
		fp->flags |= USER_FPREGS_FL_VSX;
	}
	return 0;
}

static int get_tm_regs(pid_t pid, user_fpregs_struct_t *fpregs)
{
	struct iovec iov;

	pr_debug("Dumping TM registers\n");

#define TM_REQUIRED	0
#define TM_OPTIONAL	1
#define PTRACE_GET_TM(s,n,c,u) do {					\
	iov.iov_base = &s;						\
	iov.iov_len = sizeof(s);					\
	if (ptrace(PTRACE_GETREGSET, pid, c, &iov)) {			\
		if (!u || errno != EIO) {				\
			pr_perror("Couldn't get TM "n);			\
			pr_err("Your kernel seems to not support the "	\
			       "new TM ptrace API (>= 4.8)\n");		\
			goto out_free;					\
		}							\
		pr_debug("TM "n" not supported.\n");			\
		iov.iov_base = NULL;					\
	}								\
} while(0)

	/* Get special registers */
	PTRACE_GET_TM(fpregs->tm.tm_spr_regs, "SPR", NT_PPC_TM_SPR, TM_REQUIRED);

	/* Get checkpointed regular registers */
	PTRACE_GET_TM(fpregs->tm.regs, "GPR", NT_PPC_TM_CGPR, TM_REQUIRED);

	/* Get checkpointed FP registers */
	PTRACE_GET_TM(fpregs->tm.fpregs, "FPR", NT_PPC_TM_CFPR, TM_OPTIONAL);
	if (iov.iov_base)
		fpregs->tm.flags |= USER_FPREGS_FL_FP;

	/* Get checkpointed VMX (Altivec) registers */
	PTRACE_GET_TM(fpregs->tm.vrregs, "VMX", NT_PPC_TM_CVMX, TM_OPTIONAL);
	if (iov.iov_base)
		fpregs->tm.flags |= USER_FPREGS_FL_ALTIVEC;

	/* Get checkpointed VSX registers */
	PTRACE_GET_TM(fpregs->tm.vsxregs, "VSX", NT_PPC_TM_CVSX, TM_OPTIONAL);
	if (iov.iov_base)
		fpregs->tm.flags |= USER_FPREGS_FL_VSX;

	return 0;

out_free:
	return -1;	/* still failing the checkpoint */
}

static int __get_task_regs(pid_t pid, user_regs_struct_t *regs,
			   user_fpregs_struct_t *fpregs)
{
	pr_info("Dumping GP/FPU registers for %d\n", pid);

	/*
	 * This is inspired by kernel function check_syscall_restart in
	 * arch/powerpc/kernel/signal.c
	 */
#ifndef TRAP
#define TRAP(r)              ((r).trap & ~0xF)
#endif

	if (TRAP(*regs) == 0x0C00 && regs->ccr & 0x10000000) {
		/* Restart the system call */
		switch (regs->gpr[3]) {
		case ERESTARTNOHAND:
		case ERESTARTSYS:
		case ERESTARTNOINTR:
			regs->gpr[3] = regs->orig_gpr3;
			regs->nip -= 4;
			break;
		case ERESTART_RESTARTBLOCK:
			regs->gpr[0] = __NR_restart_syscall;
			regs->nip -= 4;
			break;
		}
	}

	/* Resetting trap since we are now coming from user space. */
	regs->trap = 0;

	fpregs->flags = 0;
	/*
	 * Check for Transactional Memory operation in progress.
	 * Until we have support of TM register's state through the ptrace API,
	 * we can't checkpoint process with TM operation in progress (almost
	 * impossible) or suspended (easy to get).
	 */
	if (MSR_TM_ACTIVE(regs->msr)) {
		pr_debug("Task %d has %s TM operation at 0x%lx\n",
			 pid,
			 (regs->msr & MSR_TMS) ? "a suspended" : "an active",
			 regs->nip);
		if (get_tm_regs(pid, fpregs))
			return -1;
		fpregs->flags = USER_FPREGS_FL_TM;
	}

	if (get_fpu_regs(pid, fpregs))
		return -1;

	if (get_altivec_regs(pid, fpregs))
		return -1;

	if (fpregs->flags & USER_FPREGS_FL_ALTIVEC) {
		/*
		 * Save the VSX registers if Altivec registers are supported
		 */
		if (get_vsx_regs(pid, fpregs))
			return -1;
	}
	return 0;
}

int compel_get_task_regs(pid_t pid, user_regs_struct_t regs, save_regs_t save, void *arg)
{
	user_fpregs_struct_t fpregs;
	int ret;

	ret = __get_task_regs(pid, &regs, &fpregs);
	if (ret)
		return ret;

	return save(arg, &regs, &fpregs);
}
