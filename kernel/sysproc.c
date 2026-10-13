#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    if (stoppending(myproc())) {
      // Ctrl-Z while sleeping: stop now, keep sleeping after SIGCONT.
      release(&tickslock);
      stopcheck();
      acquire(&tickslock);
      continue;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// waitpid(pid, &status, options)
uint64
sys_waitpid(void)
{
  int pid, options;
  uint64 addr;

  argint(0, &pid);
  argaddr(1, &addr);
  argint(2, &options);
  return kwaitpid(pid, addr, options, 1);
}

// sigsend(pid, sig): pid > 0 one process, pid < 0 process group -pid.
uint64
sys_sigsend(void)
{
  int pid, sig;

  argint(0, &pid);
  argint(1, &sig);
  return ksignal(pid, sig);
}

// signal(sig, SIG_DFL or SIG_IGN)
uint64
sys_signal(void)
{
  int sig, disp;

  argint(0, &sig);
  argint(1, &disp);
  return ksigdisp(sig, disp);
}

// setpgid(pid, pgid)
uint64
sys_setpgid(void)
{
  int pid, pgid;

  argint(0, &pid);
  argint(1, &pgid);
  return ksetpgid(pid, pgid);
}

// getpgid(pid)
uint64
sys_getpgid(void)
{
  int pid;

  argint(0, &pid);
  return kgetpgid(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
