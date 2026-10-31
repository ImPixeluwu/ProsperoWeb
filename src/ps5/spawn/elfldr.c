/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

/* Derived from ps5-payload-dev/elfldr (commit 2a8da002), elfldr.c, with the
   argv/environ handling pattern of ps5-payload-dev/shsrv (6f320637).
   ProsperoBrowser changes:
     - elfldr_exec() -> elfldr_exec_fds(): instead of duplicating a single
       stdio descriptor onto 0/1/2, it keeps the deci_std* descriptors the
       rfork entry opened as 0/1/2 and injects a table of extra descriptors
       at child fd 3+i (the WebKit IPC socket lands at fd 3).
     - each injected descriptor may come from another process (src_pid,
       the UI when the spawn broker runs over TCP), and every rdup/dup2 is
       logged with its errno.
     - elfldr_spawn() -> elfldr_spawn_fds(): same signature change; also
       sets the child's environment from an envp argument (the UI's environ,
       forwarded by the spawn broker; WebKit needs the PROSPEROBROWSER_*
       variables).
     - elfldr_read()/elfldr_find_pid() dropped (not used by ProsperoBrowser).
     - the load mirror is calloc'ed (memsz gaps reach the child as zeros), a
       replaced child descriptor is logged, and a child that fails to set up
       is reaped after SIGKILL.
     - a failing sys_budget_set(0) in the rfork child is not fatal (under
       hbldr the parent is a big app and budget 0 does not exist there), and
       the child reports its stage/errno to the parent through the shared
       elfldr_spawn_report_t (see README.md, "Budget"). */

#include <elf.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/event.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <ps5/kernel.h>

#include "elfldr.h"
#include "log.h"
#include "pt.h"


#ifndef IPV6_2292PKTOPTIONS
#define IPV6_2292PKTOPTIONS 25
#endif

#define ROUND_PG(x) (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))
#define TRUNC_PG(x) ((x) & ~(PAGE_SIZE - 1))
#define PFLAGS(x)   ((((x) & PF_R) ? PROT_READ  : 0) | \
		     (((x) & PF_W) ? PROT_WRITE : 0) | \
		     (((x) & PF_X) ? PROT_EXEC  : 0))

typedef struct elfldr_ctx {
  const uint8_t* elf;
  pid_t pid;

  intptr_t base_addr;
  size_t   base_size;
  void*    base_mirror;
} elfldr_ctx_t;

static const char* SceSpZeroConf = "/system/vsh/app/NPXS40112/eboot.bin";


static int
r_relative(elfldr_ctx_t* ctx, const Elf64_Rela* rela) {
  intptr_t* loc = ctx->base_mirror + rela->r_offset;
  intptr_t val = ctx->base_addr + rela->r_addend;
  *loc = val;
  return 0;
}


static int
data_load(elfldr_ctx_t* ctx, const Elf64_Phdr* phdr) {
  void* data = ctx->base_mirror + phdr->p_vaddr;
  if(!phdr->p_memsz || !phdr->p_filesz) {
    return 0;
  }
  memcpy(data, ctx->elf + phdr->p_offset, phdr->p_filesz);
  return 0;
}


int
elfldr_sanity_check(const uint8_t* elf, size_t elf_size) {
  const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)elf;
  const Elf64_Phdr* phdr;

  if(elf_size < sizeof(Elf64_Ehdr) ||
     elf_size < sizeof(Elf64_Phdr) + ehdr->e_phoff ||
     elf_size < sizeof(Elf64_Shdr) + ehdr->e_shoff) {
    return -1;
  }

  if(ehdr->e_ident[0] != 0x7f || ehdr->e_ident[1] != 'E' ||
     ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
    return -1;
  }

  phdr = (const Elf64_Phdr*)(elf + ehdr->e_phoff);
  for(int i=0; i<ehdr->e_phnum; i++) {
    if(phdr[i].p_offset + phdr[i].p_filesz > elf_size) {
      return -1;
    }
  }

  return 0;
}


static intptr_t
elfldr_load(pid_t pid, const uint8_t* elf) {
  const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)elf;
  const Elf64_Phdr* phdr = (const Elf64_Phdr*)(elf + ehdr->e_phoff);
  const Elf64_Shdr* shdr = (const Elf64_Shdr*)(elf + ehdr->e_shoff);

  elfldr_ctx_t ctx = {.elf = elf, .pid = pid};

  size_t min_vaddr = -1;
  size_t max_vaddr = 0;
  int error = 0;

  for(int i=0; i<ehdr->e_phnum; i++) {
    if(phdr[i].p_vaddr < min_vaddr) {
      min_vaddr = phdr[i].p_vaddr;
    }
    if(max_vaddr < phdr[i].p_vaddr + phdr[i].p_memsz) {
      max_vaddr = phdr[i].p_vaddr + phdr[i].p_memsz;
    }
  }

  min_vaddr = TRUNC_PG(min_vaddr);
  max_vaddr = ROUND_PG(max_vaddr);
  ctx.base_size = max_vaddr - min_vaddr;

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  int prot = PROT_READ | PROT_WRITE;
  if(ehdr->e_type == ET_DYN) {
    ctx.base_addr = 0;
  } else if(ehdr->e_type == ET_EXEC) {
    ctx.base_addr = min_vaddr;
    flags |= MAP_FIXED;
  } else {
    LOG_PUTS("elfldr_load: ELF type not supported");
    return 0;
  }

  /* calloc, not malloc: the bytes between p_filesz and p_memsz of each
     segment (and the gaps between segments) must reach the child as zeros,
     not recycled heap contents. */
  if(!(ctx.base_mirror = calloc(1, ctx.base_size))) {
    LOG_PERROR("calloc");
    return 0;
  }

  if((ctx.base_addr = pt_mmap(pid, ctx.base_addr, ctx.base_size, prot,
			      flags, -1, 0)) == -1) {
    LOG_PT_PERROR(pid, "pt_mmap");
    free(ctx.base_mirror);
    return 0;
  }

  for(int i=0; i<ehdr->e_phnum && !error; i++) {
    if(phdr[i].p_type == PT_LOAD) {
      error = data_load(&ctx, &phdr[i]);
    }
  }

  for(int i=0; i<ehdr->e_shnum && !error; i++) {
    if(shdr[i].sh_type != SHT_RELA) {
      continue;
    }
    const Elf64_Rela* rela = (const Elf64_Rela*)(elf + shdr[i].sh_offset);
    for(size_t j=0; j<shdr[i].sh_size/sizeof(Elf64_Rela); j++) {
      if((rela[j].r_info & 0xffffffffl) == R_X86_64_RELATIVE) {
	error = r_relative(&ctx, &rela[j]);
      }
    }
  }

  if(pt_copyin(ctx.pid, ctx.base_mirror, ctx.base_addr, ctx.base_size)) {
    LOG_PERROR("pt_copyin");
    error = 1;
  }

  for(int i=0; i<ehdr->e_phnum && !error; i++) {
    if(phdr[i].p_type != PT_LOAD || phdr[i].p_memsz == 0) {
      continue;
    }
    if(phdr[i].p_flags & PF_X) {
      if(kernel_mprotect(pid, ctx.base_addr + phdr[i].p_vaddr,
			 ROUND_PG(phdr[i].p_memsz),
			 PFLAGS(phdr[i].p_flags))) {
	LOG_PERROR("kernel_mprotect");
	error = 1;
      }
    } else {
      if(pt_mprotect(pid, ctx.base_addr + phdr[i].p_vaddr,
		     ROUND_PG(phdr[i].p_memsz),
		     PFLAGS(phdr[i].p_flags))) {
	LOG_PT_PERROR(pid, "pt_mprotect");
	error = 1;
      }
    }
  }

  if(pt_msync(pid, ctx.base_addr, ctx.base_size, MS_SYNC)) {
    LOG_PT_PERROR(pid, "pt_msync");
    error = 1;
  }

  free(ctx.base_mirror);

  if(error) {
    pt_munmap(pid, ctx.base_addr, ctx.base_size);
    return 0;
  }

  return ctx.base_addr + ehdr->e_entry;
}


static intptr_t
elfldr_payload_args(pid_t pid) {
  int victim_sock;
  int master_sock;
  intptr_t buf;
  int pipe0;
  int pipe1;

  if((buf = pt_mmap(pid, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)) == -1) {
    LOG_PT_PERROR(pid, "pt_mmap");
    return 0;
  }

  if((master_sock = pt_socket(pid, AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    LOG_PT_PERROR(pid, "pt_socket");
    return 0;
  }

  pt_setint(pid, buf+0x00, 20);
  pt_setint(pid, buf+0x04, IPPROTO_IPV6);
  pt_setint(pid, buf+0x08, IPV6_TCLASS);
  pt_setint(pid, buf+0x0c, 0);
  pt_setint(pid, buf+0x10, 0);
  pt_setint(pid, buf+0x14, 0);
  if(pt_setsockopt(pid, master_sock, IPPROTO_IPV6, IPV6_2292PKTOPTIONS, buf, 24)) {
    LOG_PT_PERROR(pid, "pt_setsockopt");
    return 0;
  }

  if((victim_sock = pt_socket(pid, AF_INET6, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
    LOG_PT_PERROR(pid, "pt_socket");
    return 0;
  }

  pt_setint(pid, buf+0x00, 0);
  pt_setint(pid, buf+0x04, 0);
  pt_setint(pid, buf+0x08, 0);
  pt_setint(pid, buf+0x0c, 0);
  pt_setint(pid, buf+0x10, 0);
  if(pt_setsockopt(pid, victim_sock, IPPROTO_IPV6, IPV6_PKTINFO, buf, 20)) {
    LOG_PT_PERROR(pid, "pt_setsockopt");
    return 0;
  }

  if(kernel_overlap_sockets(pid, master_sock, victim_sock)) {
    LOG_PUTS("kernel_overlap_sockets failed");
    return 0;
  }

  if(pt_pipe(pid, buf)) {
    LOG_PT_PERROR(pid, "pt_pipe");
    return 0;
  }
  pipe0 = pt_getint(pid, buf);
  pipe1 = pt_getint(pid, buf+4);

  intptr_t args       = buf;
  intptr_t rwpipe     = buf + 0x100;
  intptr_t rwpair     = buf + 0x200;
  intptr_t kpipe_addr = kernel_get_proc_file(pid, pipe0);
  intptr_t payloadout = buf + 0x300;
  intptr_t getpid_fn  = pt_resolve(pid, "HoLVWNanBBc");

  pt_setlong(pid, args + 0x00, getpid_fn);
  pt_setlong(pid, args + 0x08, rwpipe);
  pt_setlong(pid, args + 0x10, rwpair);
  pt_setlong(pid, args + 0x18, kpipe_addr);
  pt_setlong(pid, args + 0x20, KERNEL_ADDRESS_DATA_BASE);
  pt_setlong(pid, args + 0x28, payloadout);
  pt_setint(pid, rwpipe + 0, pipe0);
  pt_setint(pid, rwpipe + 4, pipe1);
  pt_setint(pid, rwpair + 0, master_sock);
  pt_setint(pid, rwpair + 4, victim_sock);
  pt_setint(pid, payloadout, 0);

  return args;
}


static int
elfldr_prepare_exec(pid_t pid, const uint8_t* elf) {
  intptr_t entry;
  intptr_t args;
  struct reg r;

  if(pt_getregs(pid, &r)) {
    LOG_PERROR("pt_getregs");
    return -1;
  }

  if(!(entry = elfldr_load(pid, elf))) {
    LOG_PUTS("elfldr_load failed");
    return -1;
  }

  if(!(args = elfldr_payload_args(pid))) {
    LOG_PUTS("elfldr_payload_args failed");
    return -1;
  }

  pt_setlong(pid, r.r_rsp-8, r.r_rip);
  r.r_rsp -= 8;
  r.r_rip = entry;
  r.r_rdi = args;

  if(pt_setregs(pid, &r)) {
    LOG_PERROR("pt_setregs");
    return -1;
  }

  return 0;
}


static int
elfldr_set_procname(pid_t pid, const char* name) {
  intptr_t buf;
  if((buf = pt_mmap(pid, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)) == -1) {
    LOG_PT_PERROR(pid, "pt_mmap");
    return -1;
  }
  pt_copyin(pid, name, buf, strlen(name)+1);
  pt_syscall(pid, SYS_thr_set_name, -1, buf);
  pt_msync(pid, buf, PAGE_SIZE, MS_SYNC);
  pt_munmap(pid, buf, PAGE_SIZE);
  return 0;
}


/* Copy the current environment into the child and point its "environ" at it.
   Needed so the child UI/Web/Network role sees the PROSPEROBROWSER_* variables. */
static int
elfldr_set_environ(pid_t pid, char** envp) {
  size_t size = sizeof(char*);
  intptr_t environ_addr = 0;
  intptr_t envp_addr = 0;
  intptr_t pos = 0;
  int n = 0;

  if(!envp || !envp[0]) {
    return 0;
  }

  while(envp[n]) {
    size += (8 + strlen(envp[n]) + 1);
    n++;
  }
  size = ROUND_PG(size);

  if((envp_addr = pt_mmap(pid, 0, size, PROT_WRITE | PROT_READ,
			  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)) == -1) {
    LOG_PT_PERROR(pid, "pt_mmap");
    return -1;
  }

  pos = envp_addr + ((n + 1) * 8);
  for(int i=0; i<n; i++) {
    size_t len = strlen(envp[i]) + 1;
    if(pt_copyin(pid, envp[i], pos, len) ||
       pt_setlong(pid, envp_addr + (i*8), pos)) {
      LOG_PERROR("pt_copyin");
      pt_munmap(pid, envp_addr, size);
      return -1;
    }
    pos += len;
  }

  if(pt_setlong(pid, envp_addr + (n*8), 0)) {
    pt_munmap(pid, envp_addr, size);
    return -1;
  }

  if(!(environ_addr = pt_resolve(pid, "+2thxYZ4syk"))) {
    LOG_PUTS("pt_resolve(environ) failed");
    pt_munmap(pid, envp_addr, size);
    return -1;
  }

  if(pt_setlong(pid, environ_addr, envp_addr)) {
    pt_munmap(pid, envp_addr, size);
    return -1;
  }

  return 0;
}


int
elfldr_raise_privileges(pid_t pid) {
  static const uint8_t caps[16] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
				   0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
  intptr_t vnode;

  if(!(vnode = kernel_get_root_vnode())) {
    return -1;
  }
  if(kernel_set_proc_rootdir(pid, vnode) ||
     kernel_set_proc_jaildir(pid, 0) ||
     kernel_set_ucred_uid(pid, 0) ||
     kernel_set_ucred_caps(pid, caps)) {
    return -1;
  }
  return 0;
}


/* Inject fds[i].parent_fd (a descriptor of process fds[i].src_pid, or of our
   own process when src_pid is 0) into the child so it appears as
   fds[i].child_fd. The child already holds deci_std* at 0/1/2 from the rfork
   entry, which we leave in place as stdio.

   The rdup syscall runs inside the stopped child (privileges raised by the
   caller), copying the descriptor out of the source process. With the spawn
   broker on TCP the source is the UI process, which cannot pass descriptors
   over the socket (no SCM_RIGHTS): it keeps them open until the spawn
   result arrives.

   pt_rdup allocates the lowest free descriptor in the child; immediately
   after rfork that is 3, 4, ... which matches child_fd = 3+i, so in the common
   single-fd case the duplicate already sits at the target and no further work
   is needed. We still dup2+close for robustness when it does not.

   On failure, stage and err name the failing step and its errno. */
static int
elfldr_inject_fds(pid_t pid, const struct ps5_spawn_fd_entry* fds, int nfds,
		  const char** stage, int* err) {
  int tmp[PS5_SPAWN_MAX_FDS];

  for(int i=0; i<nfds; i++) {
    pid_t src = fds[i].src_pid > 0 ? fds[i].src_pid : getpid();
    tmp[i] = pt_rdup(pid, src, fds[i].parent_fd);
    if(tmp[i] < 0) {
      *err = pt_errno(pid);
      *stage = "rdup";
      ps5_log_printf("spawn: rdup(src_pid=%d, fd=%d) FAILED: errno %d (%s)",
		     (int)src, fds[i].parent_fd, *err, strerror(*err));
      for(int j=0; j<i; j++) {
	pt_close(pid, tmp[j]);
      }
      return -1;
    }
    ps5_log_printf("spawn: rdup(src_pid=%d, fd=%d) -> child fd %d",
		   (int)src, fds[i].parent_fd, tmp[i]);
  }

  for(int i=0; i<nfds; i++) {
    if(tmp[i] == fds[i].child_fd) {
      continue;
    }
    /* The child ran SceSpZeroConf's own init before we got here; if that left
       a descriptor at the target number, dup2 silently replaces it. Say so. */
    if(pt_syscall(pid, SYS_fcntl, fds[i].child_fd, F_GETFD) >= 0) {
      LOG_PRINTF("elfldr_inject_fds: child fd %d already open, replacing it "
		 "(our duplicate landed at %d)\n", fds[i].child_fd, tmp[i]);
    }
    if(pt_dup2(pid, tmp[i], fds[i].child_fd) < 0) {
      *err = pt_errno(pid);
      *stage = "dup2";
      ps5_log_printf("spawn: dup2(child fd %d -> %d) FAILED: errno %d (%s)",
		     tmp[i], fds[i].child_fd, *err, strerror(*err));
      return -1;
    }
    pt_close(pid, tmp[i]);
    ps5_log_printf("spawn: child fd %d moved to %d", tmp[i], fds[i].child_fd);
  }

  return 0;
}


int
elfldr_exec_fds(pid_t pid, const struct ps5_spawn_fd_entry* fds, int nfds,
		const uint8_t* elf, elfldr_spawn_report_t* report) {
  uint8_t caps[16];
  intptr_t jaildir;
  intptr_t rootdir;
  uint64_t authid;
  int error = 0;

  jaildir = kernel_get_proc_jaildir(pid);
  if(!(rootdir = kernel_get_proc_rootdir(pid))) {
    LOG_PUTS("kernel_get_proc_rootdir failed");
    pt_detach(pid, 0);
    return -1;
  }
  if(kernel_get_ucred_caps(pid, caps)) {
    LOG_PUTS("kernel_get_ucred_caps failed");
    pt_detach(pid, 0);
    return -1;
  }
  if(!(authid = kernel_get_ucred_authid(pid))) {
    LOG_PUTS("kernel_get_ucred_authid failed");
    pt_detach(pid, 0);
    return -1;
  }

  if(elfldr_raise_privileges(pid)) {
    LOG_PUTS("Unable to raise privileges");
    pt_detach(pid, 0);
    return -1;
  }

  if(nfds > 0 &&
     elfldr_inject_fds(pid, fds, nfds, &report->failed_stage,
		       &report->failed_errno)) {
    error = -1;
  }

  if(!error && elfldr_prepare_exec(pid, elf)) {
    error = -1;
  }

  if(kernel_set_proc_jaildir(pid, jaildir)) {
    LOG_PUTS("kernel_set_proc_jaildir failed");
    error = -1;
  }
  if(kernel_set_proc_rootdir(pid, rootdir)) {
    LOG_PUTS("kernel_set_proc_rootdir failed");
    error = -1;
  }
  if(kernel_set_ucred_caps(pid, caps)) {
    LOG_PUTS("kernel_set_ucred_caps failed");
    error = -1;
  }
  if(kernel_set_ucred_authid(pid, authid)) {
    LOG_PUTS("kernel_set_ucred_authid failed");
    error = -1;
  }

  if(pt_detach(pid, 0)) {
    LOG_PERROR("pt_detach");
    error = -1;
  }

  return error;
}


static int
elfldr_set_heap_size(pid_t pid, ssize_t size) {
  intptr_t sceLibcHeapSize;
  intptr_t sceLibcParam;
  intptr_t sceProcParam;
  intptr_t Need_sceLibc;

  if(!(sceProcParam = pt_sceKernelGetProcParam(pid))) {
    LOG_PT_PERROR(pid, "pt_sceKernelGetProcParam");
    return -1;
  }
  if(pt_copyout(pid, sceProcParam+56, &sceLibcParam, sizeof(sceLibcParam)) ||
     pt_copyout(pid, sceLibcParam+16, &sceLibcHeapSize, sizeof(sceLibcHeapSize))) {
    LOG_PERROR("pt_copyout");
    return -1;
  }
  if(pt_setlong(pid, sceLibcHeapSize, size)) {
    LOG_PERROR("pt_setlong");
    return -1;
  }
  if(size != -1) {
    return 0;
  }
  if(pt_copyout(pid, sceLibcParam+72, &Need_sceLibc, sizeof(Need_sceLibc))) {
    LOG_PERROR("pt_copyout");
    return -1;
  }
  return pt_setlong(pid, sceLibcParam+32, Need_sceLibc);
}


static int
sys_budget_set(long budget) {
  return __syscall(0x23b, budget);
}


/* Argument of elfldr_rfork_entry. The child runs on the parent's memory
   (RFMEM), so it reports into *report directly; the parent reads the fields
   only after waitpid() has seen the child stop at exec or die. */
typedef struct elfldr_rfork_args {
  char* const* argv;
  elfldr_spawn_report_t* report;
} elfldr_rfork_args_t;


static int
elfldr_rfork_fail(elfldr_rfork_args_t* args, const char* stage) {
  args->report->failed_errno = errno;
  args->report->failed_stage = stage;
  klog_perror(stage);
  return -1;
}


static int
elfldr_rfork_entry(void* ctx) {
  elfldr_rfork_args_t* args = (elfldr_rfork_args_t*)ctx;

  /* Upstream treats this as fatal. A payload started by elfldr runs in a
     process where budget 0 exists; a big app started by hbldr does not
     (ENOENT), and the child then keeps the budget it inherited. hbldr itself
     issues the same call on the big app and ignores its result. */
  if(sys_budget_set(0)) {
    args->report->budget_errno = errno ? errno : -1;
    klog_perror("sys_budget_set(0), keeping the inherited budget");
  }
  if(open("/dev/deci_stdin", O_RDONLY) < 0) {
    return elfldr_rfork_fail(args, "child open /dev/deci_stdin");
  }
  if(open("/dev/deci_stdout", O_WRONLY) < 0) {
    return elfldr_rfork_fail(args, "child open /dev/deci_stdout");
  }
  if(open("/dev/deci_stderr", O_WRONLY) < 0) {
    return elfldr_rfork_fail(args, "child open /dev/deci_stderr");
  }
  if(ptrace(PT_TRACE_ME, 0, 0, 0)) {
    return elfldr_rfork_fail(args, "child ptrace(PT_TRACE_ME)");
  }
  args->report->failed_stage = "child execve";
  execve(SceSpZeroConf, args->argv, 0);
  return elfldr_rfork_fail(args, "child execve");
}


/* Kill a child that failed to set up and reap it, so a failed spawn does
   not leave a zombie behind. */
static void
elfldr_abort(pid_t pid) {
  pt_detach(pid, SIGKILL);
  waitpid(pid, 0, 0);
}


/* Records a parent-side failure in the report and returns -1. */
static pid_t
elfldr_spawn_fail(elfldr_spawn_report_t* report, const char* stage, int err) {
  report->failed_stage = stage;
  report->failed_errno = err;
  return -1;
}


pid_t
elfldr_spawn_fds(char* const argv[], char* const envp[],
		 const struct ps5_spawn_fd_entry* fds,
		 int nfds, const uint8_t* elf, size_t elf_size,
		 elfldr_spawn_report_t* report) {
  elfldr_rfork_args_t args;
  intptr_t libkernel_entry;
  off_t need_reloc_offset;
  intptr_t eboot_entry;
  off_t main_offset;
  struct kevent evt;
  int status = 0;
  void* stack;
  pid_t pid;
  int kq;

  memset(report, 0, sizeof(*report));

  if(elfldr_sanity_check(elf, elf_size)) {
    LOG_PUTS("elfldr_sanity_check failed");
    return elfldr_spawn_fail(report, "elfldr_sanity_check", 0);
  }

  if((kq = kqueue()) < 0) {
    LOG_PERROR("kqueue");
    return elfldr_spawn_fail(report, "kqueue", errno);
  }

  if(!(stack = malloc(PAGE_SIZE))) {
    LOG_PERROR("malloc");
    close(kq);
    return elfldr_spawn_fail(report, "malloc stack", errno);
  }

  args.argv = argv;
  args.report = report;
  if((pid = rfork_thread(RFPROC | RFCFDG | RFMEM, stack+PAGE_SIZE-8,
			 elfldr_rfork_entry, &args)) < 0) {
    int err = errno;
    LOG_PERROR("rfork_thread");
    free(stack);
    close(kq);
    return elfldr_spawn_fail(report, "rfork_thread", err);
  }

  /* A child that already exited cannot be registered (ESRCH); waitpid()
     still returns for both outcomes (exec trap stop, or exit), so a kevent
     failure is logged but does not skip the wait. */
  EV_SET(&evt, pid, EVFILT_PROC, EV_ADD, NOTE_EXEC, 0, 0);
  if(kevent(kq, &evt, 1, &evt, 1, 0) < 0) {
    LOG_PERROR("kevent");
  }
  if(waitpid(pid, &status, 0) < 0) {
    int err = errno;
    LOG_PERROR("waitpid");
    free(stack);
    close(kq);
    return elfldr_spawn_fail(report, "waitpid", err);
  }

  free(stack);
  close(kq);

  /* A child that failed before execve exited instead of stopping at the
     exec trap; report_t already holds its stage and errno. */
  if(!WIFSTOPPED(status)) {
    report->child_status = status;
    if(!report->failed_stage) {
      report->failed_stage = "child exited before exec";
    }
    return -1;
  }
  report->failed_stage = NULL;
  report->failed_errno = 0;

  if(!(libkernel_entry = kernel_dynlib_entry_addr(pid, 1)) ||
     !(eboot_entry = kernel_dynlib_entry_addr(pid, 0))) {
    LOG_PUTS("kernel_dynlib_entry_addr failed");
    elfldr_abort(pid);
    return elfldr_spawn_fail(report, "kernel_dynlib_entry_addr", 0);
  }

  if(kernel_get_fw_version() < 0x14000000) {
    need_reloc_offset = 15;
    main_offset = 58;
  } else {
    need_reloc_offset = 15;
    main_offset = 63;
  }

  if(pt_breakat(pid, libkernel_entry + need_reloc_offset)) {
    int err = errno;
    LOG_PERROR("pt_breakat");
    elfldr_abort(pid);
    return elfldr_spawn_fail(report, "pt_breakat libkernel", err);
  }

  elfldr_set_heap_size(pid, -1);

  if(pt_breakat(pid, eboot_entry + main_offset)) {
    int err = errno;
    LOG_PERROR("pt_breakat");
    elfldr_abort(pid);
    return elfldr_spawn_fail(report, "pt_breakat main", err);
  }

  elfldr_set_procname(pid, argv[0]);
  elfldr_set_environ(pid, (char**)envp);

  if(elfldr_exec_fds(pid, fds, nfds, elf, report)) {
    kill(pid, SIGKILL);
    waitpid(pid, 0, 0);
    if(report->failed_stage) {
      return -1;  /* the descriptor injection named its stage */
    }
    return elfldr_spawn_fail(report, "elfldr_exec_fds", 0);
  }

  return pid;
}
