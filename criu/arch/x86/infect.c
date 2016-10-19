#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include "asm/parasite-syscall.h"
#include "uapi/std/syscall-codes.h"
#include "err.h"
#include "asm/fpu.h"
#include "asm/types.h"
#include "errno.h"
#include "asm/cpu.h"
#include "parasite-syscall.h"
#include "infect.h"
#include "infect-priv.h"

/*
 * Injected syscall instruction
 */
const char code_syscall[] = {
	0x0f, 0x05,				/* syscall    */
	0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc	/* int 3, ... */
};

const char code_int_80[] = {
	0xcd, 0x80,				/* int $0x80  */
	0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc	/* int 3, ... */
};

static const int
code_syscall_aligned = round_up(sizeof(code_syscall), sizeof(long));
static const int
code_int_80_aligned = round_up(sizeof(code_syscall), sizeof(long));

static inline __always_unused void __check_code_syscall(void)
{
	BUILD_BUG_ON(code_int_80_aligned != BUILTIN_SYSCALL_SIZE);
	BUILD_BUG_ON(code_syscall_aligned != BUILTIN_SYSCALL_SIZE);
	BUILD_BUG_ON(!is_log2(sizeof(code_syscall)));
}

#define get_signed_user_reg(pregs, name)				\
	((user_regs_native(pregs)) ? (int64_t)((pregs)->native.name) :	\
				(int32_t)((pregs)->compat.name))

int compel_get_task_regs(pid_t pid, user_regs_struct_t regs, save_regs_t save, void *arg)
{
	user_fpregs_struct_t xsave	= {  }, *xs = NULL;

	struct iovec iov;
	int ret = -1;

	pr_info("Dumping general registers for %d in %s mode\n", pid,
			user_regs_native(&regs) ? "native" : "compat");

	/* Did we come from a system call? */
	if (get_signed_user_reg(&regs, orig_ax) >= 0) {
		/* Restart the system call */
		switch (get_signed_user_reg(&regs, ax)) {
		case -ERESTARTNOHAND:
		case -ERESTARTSYS:
		case -ERESTARTNOINTR:
			set_user_reg(&regs, ax, get_user_reg(&regs, orig_ax));
			set_user_reg(&regs, ip, get_user_reg(&regs, ip) - 2);
			break;
		case -ERESTART_RESTARTBLOCK:
			pr_warn("Will restore %d with interrupted system call\n", pid);
			set_user_reg(&regs, ax, -EINTR);
			break;
		}
	}

#ifndef PTRACE_GETREGSET
# define PTRACE_GETREGSET 0x4204
#endif

	if (!cpu_has_feature(X86_FEATURE_FPU))
		goto out;

	/*
	 * FPU fetched either via fxsave or via xsave,
	 * thus decode it accrodingly.
	 */

	pr_info("Dumping GP/FPU registers for %d\n", pid);

	if (cpu_has_feature(X86_FEATURE_XSAVE)) {
		iov.iov_base = &xsave;
		iov.iov_len = sizeof(xsave);

		if (ptrace(PTRACE_GETREGSET, pid, (unsigned int)NT_X86_XSTATE, &iov) < 0) {
			pr_perror("Can't obtain FPU registers for %d", pid);
			goto err;
		}
	} else {
		if (ptrace(PTRACE_GETFPREGS, pid, NULL, &xsave)) {
			pr_perror("Can't obtain FPU registers for %d", pid);
			goto err;
		}
	}

	xs = &xsave;
out:
	ret = save(arg, &regs, xs);
err:
	return ret;
}

int compel_syscall(struct parasite_ctl *ctl, int nr, unsigned long *ret,
		unsigned long arg1,
		unsigned long arg2,
		unsigned long arg3,
		unsigned long arg4,
		unsigned long arg5,
		unsigned long arg6)
{
	user_regs_struct_t regs = ctl->orig.regs;
	int err;

	if (user_regs_native(&regs)) {
		user_regs_struct64 *r = &regs.native;

		r->ax  = (uint64_t)nr;
		r->di  = arg1;
		r->si  = arg2;
		r->dx  = arg3;
		r->r10 = arg4;
		r->r8  = arg5;
		r->r9  = arg6;

		err = compel_execute_syscall(ctl, &regs, code_syscall);
	} else {
		user_regs_struct32 *r = &regs.compat;

		r->ax  = (uint32_t)nr;
		r->bx  = arg1;
		r->cx  = arg2;
		r->dx  = arg3;
		r->si  = arg4;
		r->di  = arg5;
		r->bp  = arg6;

		err = compel_execute_syscall(ctl, &regs, code_int_80);
	}

	*ret = get_user_reg(&regs, ax);
	return err;
}

void *remote_mmap(struct parasite_ctl *ctl,
		  void *addr, size_t length, int prot,
		  int flags, int fd, off_t offset)
{
	unsigned long map;
	int err;
	bool compat_task = !user_regs_native(&ctl->orig.regs);

	err = compel_syscall(ctl, __NR(mmap, compat_task), &map,
			(unsigned long)addr, length, prot, flags, fd, offset);
	if (err < 0)
		return NULL;

	if (IS_ERR_VALUE(map)) {
		if (map == -EACCES && (prot & PROT_WRITE) && (prot & PROT_EXEC))
			pr_warn("mmap(PROT_WRITE | PROT_EXEC) failed for %d, "
				"check selinux execmem policy\n", ctl->rpid);
		return NULL;
	}

	return (void *)map;
}
