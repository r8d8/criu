#ifndef __CR_ASM_DUMP_H__
#define __CR_ASM_DUMP_H__

typedef int (*save_regs_t)(void *, user_regs_struct_t *, user_fpregs_struct_t *);
extern int save_task_regs(void *, user_regs_struct_t *, user_fpregs_struct_t *);
extern int get_task_regs(pid_t pid, user_regs_struct_t regs, save_regs_t, void *);
extern int arch_alloc_thread_info(CoreEntry *core);
extern void arch_free_thread_info(CoreEntry *core);


static inline void core_put_tls(CoreEntry *core, tls_t tls)
{
	core->ti_arm->tls = tls;
}

#endif
