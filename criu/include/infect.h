#ifndef __COMPEL_INFECT_H__
#define __COMPEL_INFECT_H__
extern int compel_stop_task(int pid);

struct seize_task_status {
	char			state;
	int			ppid;
	unsigned long long	sigpnd;
	unsigned long long	shdpnd;
	int			seccomp_mode;
};

extern int compel_wait_task(int pid, int ppid,
		int (*get_status)(int pid, struct seize_task_status *),
		struct seize_task_status *st);

/*
 * FIXME -- these should be mapped to pid.h's
 */

#define TASK_ALIVE		0x1
#define TASK_DEAD		0x2
#define TASK_STOPPED		0x3
#define TASK_ZOMBIE		0x6

struct parasite_ctl;
struct thread_ctx;

extern struct parasite_ctl *compel_prepare(int pid);
extern int compel_infect(struct parasite_ctl *ctl, unsigned long nr_threads, unsigned long args_size);
extern int compel_prepare_thread(int pid, struct thread_ctx *ctx);

#endif
