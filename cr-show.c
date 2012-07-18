#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>

#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "types.h"
#include "list.h"
#include "namespaces.h"
#include "compiler.h"
#include "crtools.h"
#include "util.h"
#include "sockets.h"
#include "image.h"
#include "uts_ns.h"
#include "ipc_ns.h"
#include "pstree.h"

#include "protobuf.h"
#include "protobuf/fdinfo.pb-c.h"
#include "protobuf/regfile.pb-c.h"
#include "protobuf/ghost-file.pb-c.h"
#include "protobuf/fifo.pb-c.h"
#include "protobuf/remap-file-path.pb-c.h"
#include "protobuf/fown.pb-c.h"
#include "protobuf/fs.pb-c.h"
#include "protobuf/pstree.pb-c.h"
#include "protobuf/pipe.pb-c.h"
#include "protobuf/pipe-data.pb-c.h"
#include "protobuf/sa.pb-c.h"
#include "protobuf/itimer.pb-c.h"
#include "protobuf/mm.pb-c.h"

#define DEF_PAGES_PER_LINE	6

#ifndef CONFIG_X86_64
# error No x86-32 support yet
#endif


#define PR_SYMBOL(sym)			\
	(isprint(sym) ? sym : '.')

#define pr_regs4(s, n1, n2, n3, n4)	\
	pr_msg("%8s: 0x%16lx "		\
	       "%8s: 0x%16lx "		\
	       "%8s: 0x%16lx "		\
	       "%8s: 0x%16lx\n",	\
	       #n1, s.n1,		\
	       #n2, s.n2,		\
	       #n3, s.n3,		\
	       #n4, s.n4)

#define pr_regs3(s, n1, n2, n3)		\
	pr_msg("%8s: 0x%16lx "		\
	       "%8s: 0x%16lx "		\
	       "%8s: 0x%16lx\n",	\
	       #n1, s.n1,		\
	       #n2, s.n2,		\
	       #n3, s.n3)

static char local_buf[PAGE_SIZE];
static LIST_HEAD(pstree_list);

static char *fdtype2s(u8 type)
{
	static char und[4];
	static char *fdtypes[] = {
		[FDINFO_REG] = "reg",
		[FDINFO_INETSK] = "isk",
		[FDINFO_PIPE] = "pipe",
		[FDINFO_FIFO] = "fifo",
		[FDINFO_UNIXSK] = "usk",
		[FDINFO_EVENTFD] = "efd",
		[FDINFO_EVENTPOLL] = "epl",
		[FDINFO_INOTIFY] = "ify",
	};

	if (type > FDINFO_UND && type < FD_INFO_MAX)
		return fdtypes[type];
	snprintf(und, sizeof(und), "x%03d\n", (int)type);
	return und;
}

void show_files(int fd_files, struct cr_options *o)
{
	pr_img_head(CR_FD_FDINFO);

	while (1) {
		FdinfoEntry *e;
		int ret = pb_read_eof(fd_files, &e, fdinfo_entry);
		if (ret <= 0)
			goto out;
		pr_msg("type: %-5s fd: %-5d id: %#x flags %#x\n",
		       fdtype2s(e->type), e->fd, e->id, e->flags);
		fdinfo_entry__free_unpacked(e, NULL);
	}

out:
	pr_img_tail(CR_FD_FDINFO);
}

void show_fown_cont(fown_t *fown)
{
	pr_msg("fown: uid: %#x euid: %#x signum: %#x pid_type: %#x pid: %u",
	       fown->uid, fown->euid, fown->signum, fown->pid_type, fown->pid);
}

/* FIXME Drop once PB transition complete */
void pb_show_fown_cont(void *p)
{
	FownEntry *fown = p;
	pr_msg("fown: uid: %#x euid: %#x signum: %#x pid_type: %#x pid: %u",
	       fown->uid, fown->euid, fown->signum, fown->pid_type, fown->pid);
}

void show_reg_files(int fd_reg_files, struct cr_options *o)
{
	local_buf[0] = 0;

	pr_img_head(CR_FD_REG_FILES);
	while (1) {
		RegFileEntry *rfe;
		int ret;

		ret = pb_read_eof(fd_reg_files, &rfe, reg_file_entry);
		if (ret <= 0)
			break;

		pr_msg("id: 0x%8x flags: 0x%4x pos: 0x%lx",
		       rfe->id, rfe->flags, rfe->pos);
		if (rfe->name)
			pr_msg(" --> %s", rfe->name);
		pr_msg("\n");
		reg_file_entry__free_unpacked(rfe, NULL);
	}
	pr_img_tail(CR_FD_REG_FILES);
}

static inline char *remap_id_type(u32 id)
{
	if (id & REMAP_GHOST)
		return "ghost";
	else
		return "real";
}

void show_remap_files(int fd, struct cr_options *o)
{
	RemapFilePathEntry *rfe;

	pr_img_head(CR_FD_REMAP_FPATH);

	while (1) {
		int ret;

		ret = pb_read_eof(fd, &rfe, remap_file_path_entry);
		if (ret <= 0)
			break;

		pr_msg("%#x -> %#x (%s)\n", rfe->orig_id,
				(rfe->remap_id & ~REMAP_GHOST),
				remap_id_type(rfe->remap_id));
		remap_file_path_entry__free_unpacked(rfe, NULL);
	}

	pr_img_tail(CR_FD_REMAP_FPATH);
}

void show_ghost_file(int fd, struct cr_options *o)
{
	GhostFileEntry *gfe;

	pr_img_head(CR_FD_GHOST_FILE);
	if (pb_read_eof(fd, &gfe, ghost_file_entry) > 0) {
		pr_msg("uid %u god %u mode %#x\n", gfe->uid, gfe->gid, gfe->mode);
		ghost_file_entry__free_unpacked(gfe, NULL);
	}
	pr_img_tail(CR_FD_GHOST_FILE);
}

void __show_pipes_data(int fd, struct cr_options *o)
{
	PipeDataEntry *e;

	while (1) {
		if (pb_read_eof(fd, &e, pipe_data_entry) <= 0)
			break;
		pr_msg("pipeid: 0x%8x bytes: 0x%8x\n", e->pipe_id, e->bytes);
		lseek(fd, e->bytes, SEEK_CUR);
		pipe_data_entry__free_unpacked(e, NULL);
	}
}

void show_pipes_data(int fd_pipes, struct cr_options *o)
{
	pr_img_head(CR_FD_PIPES_DATA);
	__show_pipes_data(fd_pipes, o);
	pr_img_tail(CR_FD_PIPES_DATA);
}

void show_pipes(int fd_pipes, struct cr_options *o)
{
	PipeEntry *e;
	int ret;

	pr_img_head(CR_FD_PIPES);

	while (1) {
		ret = pb_read_eof(fd_pipes, &e, pipe_entry);
		if (ret <= 0)
			goto out;
		pr_msg("id: 0x%8x pipeid: 0x%8x flags: 0x%8x ",
		       e->id, e->pipe_id, e->flags);
		pb_show_fown_cont(e->fown);
		pr_msg("\n");
		pipe_entry__free_unpacked(e, NULL);
	}

out:
	pr_img_tail(CR_FD_PIPES);
}

void show_fifo_data(int fd, struct cr_options *o)
{
	pr_img_head(CR_FD_FIFO_DATA);
	__show_pipes_data(fd, o);
	pr_img_tail(CR_FD_FIFO_DATA);
}

void show_fifo(int fd, struct cr_options *o)
{
	FifoEntry *e;

	pr_img_head(CR_FD_FIFO);
	while (1) {
		if (pb_read_eof(fd, &e, fifo_entry) <= 0)
			break;
		pr_msg("id: 0x%8x pipeid: 0x%8x\n", e->id, e->pipe_id);
		fifo_entry__free_unpacked(e, NULL);
	}
	pr_img_tail(CR_FD_FIFO);
}

void show_fs(int fd_fs, struct cr_options *o)
{
	FsEntry *fe;

	pr_img_head(CR_FD_FS);

	if (pb_read_eof(fd_fs, &fe, fs_entry) > 0) {
		pr_msg("CWD : %#x\n", fe->cwd_id);
		pr_msg("ROOT: %#x\n", fe->root_id);
		fs_entry__free_unpacked(fe, NULL);
	}

	pr_img_tail(CR_FD_FS);
}

void show_vmas(int fd_vma, struct cr_options *o)
{
	struct vma_area vma_area = {};
	struct vma_entry ve;

	pr_img_head(CR_FD_VMAS);

	while (1) {
		int ret;

		ret = read_img_eof(fd_vma, &ve);
		if (ret <= 0)
			break;

		/* Simply in a sake of fancy printing */
		vma_area.vma = ve;
		pr_msg_vma(&vma_area);
	}

	pr_img_tail(CR_FD_VMAS);
}

static int nice_width_for(unsigned long addr)
{
	int ret = 3;

	while (addr) {
		addr >>= 4;
		ret++;
	}

	return ret;
}

void print_data(unsigned long addr, unsigned char *data, size_t size)
{
	int i, j, addr_len;

	addr_len = nice_width_for(addr + size);

	for (i = 0; i < size; i+= 16) {
		pr_msg("%#0*lx: ", addr_len, addr + i);
		for (j = 0; j < 8; j++)
			pr_msg("0x%02x ", data[i +  j]);
		pr_msg(" ");
		for (j = 8; j < 16; j++)
			pr_msg("0x%02x ", data[i +  j]);

		pr_msg(" |");
		for (j = 0; j < 8; j++)
			pr_msg("%c ", PR_SYMBOL(data[i + j]));
		pr_msg(" ");
		for (j = 8; j < 16; j++)
			pr_msg("%c ", PR_SYMBOL(data[i + j]));

		pr_msg("|\n");
	}
}

void show_pages(int fd_pages, struct cr_options *o)
{
	pr_img_head(CR_FD_PAGES);

	if (o->show_pages_content) {
		while (1) {
			struct page_entry e;

			if (read_img_eof(fd_pages, &e) <= 0)
				break;

			print_data(e.va, e.data, PAGE_IMAGE_SIZE);
			pr_msg("\n                  --- End of page ---\n\n");
		}
	} else {
		while (1) {
			struct page_entry e;
			int i;

			pr_msg("\t");
			for (i = 0; i < DEF_PAGES_PER_LINE; i++) {
				if (read_img_eof(fd_pages, &e) <= 0) {
					pr_msg("\n");
					goto out;
				}

				pr_msg("0x%16lx ", e.va);
			}
			pr_msg("\n");
		}
	}

out:
	pr_img_tail(CR_FD_PAGES);
}

void show_sigacts(int fd_sigacts, struct cr_options *o)
{
	SaEntry *e;

	pr_img_head(CR_FD_SIGACT);

	while (1) {
		int ret;

		ret = pb_read_eof(fd_sigacts, &e, sa_entry);
		if (ret <= 0)
			goto out;
		pr_msg("sigaction: 0x%016lx mask: 0x%08lx "
		       "flags: 0x%016lx restorer: 0x%016lx\n",
		       (long)e->sigaction,
		       (long)e->mask,
		       (long)e->flags,
		       (long)e->restorer);
		sa_entry__free_unpacked(e, NULL);
	}

out:
	pr_img_tail(CR_FD_SIGACT);
}

static void show_itimer(char *n, ItimerEntry *ie)
{
	pr_msg("%s: int %lu.%lu val %lu.%lu\n", n,
	       (unsigned long)ie->isec, (unsigned long)ie->iusec,
	       (unsigned long)ie->vsec, (unsigned long)ie->vusec);
}

void show_itimers(int fd, struct cr_options *o)
{
	ItimerEntry *ie;
	int ret;

	pr_img_head(CR_FD_ITIMERS);

	ret = pb_read(fd, &ie, itimer_entry);
	if (ret < 0)
		goto out;
	show_itimer("real", ie);
	itimer_entry__free_unpacked(ie, NULL);

	ret = pb_read(fd, &ie, itimer_entry);
	if (ret < 0)
		goto out;
	show_itimer("virt", ie);
	itimer_entry__free_unpacked(ie, NULL);

	ret = pb_read(fd, &ie, itimer_entry);
	if (ret < 0)
		goto out;
	show_itimer("prof", ie);
	itimer_entry__free_unpacked(ie, NULL);
out:
	pr_img_tail(CR_FD_ITIMERS);
}

static void show_cap(char *name, u32 *v)
{
	int i;

	pr_msg("%s: ", name);
	for (i = CR_CAP_SIZE - 1; i >= 0; i--)
		pr_msg("0x%08x", v[i]);
	pr_msg("\n");
}

void show_creds(int fd, struct cr_options *o)
{
	struct creds_entry ce;

	pr_img_head(CR_FD_CREDS);
	if (read_img(fd, &ce) < 0)
		goto out;

	pr_msg("uid %u  euid %u  suid %u  fsuid %u\n",
	       ce.uid, ce.euid, ce.suid, ce.fsuid);
	pr_msg("gid %u  egid %u  sgid %u  fsgid %u\n",
	       ce.gid, ce.egid, ce.sgid, ce.fsgid);

	show_cap("Inh", ce.cap_inh);
	show_cap("Eff", ce.cap_eff);
	show_cap("Prm", ce.cap_prm);
	show_cap("Bnd", ce.cap_bnd);

	pr_msg("secbits: %#x\n", ce.secbits);
out:
	pr_img_tail(CR_FD_CREDS);
}

static int show_collect_pstree(int fd_pstree, struct list_head *collect)
{
	PstreeEntry *e;

	pr_img_head(CR_FD_PSTREE);

	while (1) {
		int ret;
		struct pstree_item *item = NULL;

		e = NULL;
		ret = pb_read_eof(fd_pstree, &e, pstree_entry);
		if (ret <= 0)
			goto out;
		pr_msg("pid: %8d ppid %8d pgid: %8d sid %8d  n_threads: %8d\n",
		       (int)e->pid, (int)e->ppid, (int)e->pgid,
		       (int)e->sid, (int)e->n_threads);

		if (collect) {
			item = xzalloc(sizeof(struct pstree_item));
			if (!item)
				return -1;

			item->pid.virt = e->pid;
			item->nr_threads = e->n_threads;
			item->threads = xzalloc(sizeof(u32) * e->n_threads);
			if (!item->threads) {
				xfree(item);
				return -1;
			}

			list_add_tail(&item->list, collect);
		}

		if (e->n_threads) {
			pr_msg("  \\\n");
			pr_msg("   --- threads: ");
			while (e->n_threads--) {
				pr_msg(" %6d", (int)e->threads[e->n_threads]);
				if (item)
					item->threads[e->n_threads].virt = e->threads[e->n_threads];
			}
			pr_msg("\n");
		}

		pstree_entry__free_unpacked(e, NULL);
	}

out:
	if (e)
		pstree_entry__free_unpacked(e, NULL);
	pr_img_tail(CR_FD_PSTREE);
	return 0;
}

void show_pstree(int fd_pstree, struct cr_options *o)
{
	show_collect_pstree(fd_pstree, NULL);
}

static void show_core_regs(int fd_core)
{
	struct user_regs_entry regs;

	pr_msg("\n\t---[GP registers set]---\n");

	lseek(fd_core, GET_FILE_OFF(struct core_entry, arch.gpregs), SEEK_SET);

	if (read_img(fd_core, &regs) < 0)
		goto err;

	pr_regs4(regs, cs, ip, ds, es);
	pr_regs4(regs, ss, sp, fs, gs);
	pr_regs4(regs, di, si, dx, cx);
	pr_regs4(regs, ax, r8, r9, r10);
	pr_regs4(regs, r11, r12, r13, r14);
	pr_regs3(regs, r15, bp, bx);
	pr_regs4(regs, orig_ax, flags, fs_base, gs_base);
	pr_msg("\n");

err:
	return;
}

static inline char *task_state_str(int state)
{
	switch (state) {
	case TASK_ALIVE:
		return "running/sleeping";
	case TASK_DEAD:
		return "zombie";
	default:
		return "UNKNOWN";
	}
}

static void show_core_rest(int fd_core)
{
	struct task_core_entry tc;

	lseek(fd_core, GET_FILE_OFF(struct core_entry, tc), SEEK_SET);
	if (read_img(fd_core, &tc) < 0)
		goto err;

	pr_msg("\n\t---[Task parameters]---\n");
	pr_msg("\tPersonality:  %#x\n", tc.personality);
	pr_msg("\tCommand:      %s\n", tc.comm);
	pr_msg("\tState:        %d (%s)\n",
	       (int)tc.task_state,
	       task_state_str((int)tc.task_state));

	pr_msg("\t   Exit code: %u\n",
	       (unsigned int)tc.exit_code);

	pr_msg("\tBlkSig: 0x%lx\n", tc.blk_sigset);
	pr_msg("\n");

err:
	return;
}

static void show_core_ids(int fd)
{
	struct core_ids_entry cie;

	lseek(fd, GET_FILE_OFF(struct core_entry, ids), SEEK_SET);
	if (read_img(fd, &cie) < 0)
		goto err;

	pr_msg("\tVM:      %#x\n", cie.vm_id);
	pr_msg("\tFS:      %#x\n", cie.fs_id);
	pr_msg("\tFILES:   %#x\n", cie.files_id);
	pr_msg("\tSIGHAND: %#x\n", cie.sighand_id);
err:
	return;
}

void show_core(int fd_core, struct cr_options *o)
{
	struct stat stat;
	bool is_thread;

	if (fstat(fd_core, &stat)) {
		pr_perror("Can't get stat on core file");
		goto out;
	}

	is_thread = (stat.st_size == GET_FILE_OFF_AFTER(struct core_entry));

	if (is_thread)
		pr_img_head(CR_FD_CORE, " (thread)");
	else
		pr_img_head(CR_FD_CORE);

	show_core_regs(fd_core);
	show_core_rest(fd_core);
	show_core_ids(fd_core);
out:
	pr_img_tail(CR_FD_CORE);
}

void show_mm(int fd_mm, struct cr_options *o)
{
	MmEntry *mme;

	pr_img_head(CR_FD_MM);

	if (pb_read(fd_mm, &mme, mm_entry) < 0)
		goto out;

	pr_msg("\tBrk:          0x%lx\n", mme->mm_brk);
	pr_msg("\tStart code:   0x%lx\n", mme->mm_start_code);
	pr_msg("\tEnd code:     0x%lx\n", mme->mm_end_code);
	pr_msg("\tStart stack:  0x%lx\n", mme->mm_start_stack);
	pr_msg("\tStart data:   0x%lx\n", mme->mm_start_data);
	pr_msg("\tEnd data:     0x%lx\n", mme->mm_end_data);
	pr_msg("\tStart brk:    0x%lx\n", mme->mm_start_brk);
	pr_msg("\tArg start:    0x%lx\n", mme->mm_arg_start);
	pr_msg("\tArg end:      0x%lx\n", mme->mm_arg_end);
	pr_msg("\tEnv start:    0x%lx\n", mme->mm_env_start);
	pr_msg("\tEnv end:      0x%lx\n", mme->mm_env_end);
	pr_msg("\tExe file ID   %#x\n", mme->exe_file_id);

	mm_entry__free_unpacked(mme, NULL);
out:
	pr_img_tail(CR_FD_MM);
}

static int cr_parse_file(struct cr_options *opts)
{
	u32 magic;
	int fd = -1, ret = -1, i;

	fd = open(opts->show_dump_file, O_RDONLY);
	if (fd < 0) {
		pr_perror("Can't open %s", opts->show_dump_file);
		goto err;
	}

	if (read_img(fd, &magic) < 0)
		goto err;

	for (i = 0; i < CR_FD_MAX; i++)
		if (fdset_template[i].magic == magic)
			break;

	if (i == CR_FD_MAX) {
		pr_err("Unknown magic %#x in %s\n",
				magic, opts->show_dump_file);
		goto err;
	}

	if (!fdset_template[i].show) {
		pr_err("No handler for %#x/%s\n",
				magic, opts->show_dump_file);
		goto err;
	}

	fdset_template[i].show(fd, opts);
	ret = 0;
err:
	close_safe(&fd);
	return ret;
}

static int cr_show_all(struct cr_options *opts)
{
	struct pstree_item *item = NULL, *tmp;
	LIST_HEAD(pstree_list);
	int i, ret = -1, fd, pid;

	fd = open_image_ro(CR_FD_PSTREE);
	if (fd < 0)
		goto out;

	ret = show_collect_pstree(fd, &pstree_list);
	if (ret)
		goto out;

	close(fd);

	fd = open_image_ro(CR_FD_SK_QUEUES);
	if (fd < 0)
		goto out;

	show_sk_queues(fd, opts);
	close(fd);

	pid = list_first_entry(&pstree_list, struct pstree_item, list)->pid.virt;
	ret = try_show_namespaces(pid, opts);
	if (ret)
		goto out;

	list_for_each_entry(item, &pstree_list, list) {
		struct cr_fdset *cr_fdset = NULL;

		cr_fdset = cr_task_fdset_open(item->pid.virt, O_SHOW);
		if (!cr_fdset)
			goto out;

		show_core(fdset_fd(cr_fdset, CR_FD_CORE), opts);

		if (item->nr_threads > 1) {
			int fd_th;

			for (i = 0; i < item->nr_threads; i++) {

				if (item->threads[i].virt == item->pid.virt)
					continue;

				fd_th = open_image_ro(CR_FD_CORE, item->threads[i]);
				if (fd_th < 0)
					goto out;

				pr_msg("\n");
				pr_msg("Thread: %d\n", item->threads[i].virt);
				pr_msg("----------------------------------------\n");

				show_core(fd_th, opts);

				pr_msg("----------------------------------------\n");

			}
		}

		for (i = _CR_FD_TASK_FROM + 1; i < _CR_FD_TASK_TO; i++)
			if (i != CR_FD_CORE && fdset_template[i].show)
				fdset_template[i].show(fdset_fd(cr_fdset, i), opts);

		close_cr_fdset(&cr_fdset);

		if (opts->leader_only)
			break;
	}

out:
	list_for_each_entry_safe(item, tmp, &pstree_list, list) {
		list_del(&item->list);
		xfree(item->threads);
		xfree(item);
	}
	return ret;
}

int cr_show(struct cr_options *opts)
{
	if (opts->show_dump_file)
		return cr_parse_file(opts);

	return cr_show_all(opts);
}
