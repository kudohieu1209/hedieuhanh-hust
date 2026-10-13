#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "signal.h"
#include "wait.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->pgid = 0;
  p->pendstop = 0;
  p->stopsig = 0;
  p->sigignore = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  p->pgid = p->pid; // init leads process group 1

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if (sz + n > TRAPFRAME) {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  // a child starts in its parent's process group and keeps
  // the parent's list of ignored signals.
  np->pgid = p->pgid;
  np->sigignore = p->sigignore;

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      // nobody is left to continue a stopped orphan (init never
      // does job control), so terminate it like Unix does with SIGHUP.
      acquire(&pp->lock);
      if (pp->state == STOPPED)
        sigdeliver(pp, SIGHUP);
      release(&pp->lock);
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to change state and return its pid.
//   pid == -1: any child; pid > 0: that child; pid < -1: any child
//              in process group -pid.
//   options:   WNOHANG (return 0 instead of blocking),
//              WUNTRACED (also report children that stopped).
//   encode:    0 = copy out the raw exit code (old wait() behaviour),
//              1 = copy out a status word decoded by the W*() macros.
// Return -1 if there is no matching child.
int
kwaitpid(int pid, uint64 addr, int options, int encode)
{
  struct proc *pp;
  int havekids, cpid, status;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for children that changed state.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent != p)
        continue;
      // make sure the child isn't still in exit() or swtch().
      acquire(&pp->lock);
      if ((pid > 0 && pp->pid != pid) || (pid < -1 && pp->pgid != -pid)) {
        release(&pp->lock);
        continue;
      }
      havekids = 1;

      if (pp->state == ZOMBIE) {
        // Found one that exited.
        cpid = pp->pid;
        if (!encode)
          status = pp->xstate;
        else if (pp->killed)
          status = pp->killed & 0x7f;  // killed by a signal
        else
          status = (pp->xstate & 0xff) << 8;
        if (addr != 0 &&
            copyout(p->pagetable, p->sz, addr, (char *)&status,
                    sizeof(status)) < 0) {
          release(&pp->lock);
          release(&wait_lock);
          return -1;
        }
        pp->parent = 0;
        freeproc(pp);
        release(&pp->lock);
        release(&wait_lock);
        return cpid;
      }

      if ((options & WUNTRACED) && pp->state == STOPPED && pp->stopsig) {
        // Found one that stopped and hasn't been reported yet.
        cpid = pp->pid;
        status = (pp->stopsig << 8) | 0x7f;
        pp->stopsig = 0; // report each stop only once
        release(&pp->lock);
        release(&wait_lock);
        if (addr != 0 &&
            copyout(p->pagetable, p->sz, addr, (char *)&status,
                    sizeof(status)) < 0)
          return -1;
        return cpid;
      }
      release(&pp->lock);
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    if (options & WNOHANG) {
      release(&wait_lock);
      return 0;
    }

    // We may have been told to stop (e.g. a shell running a
    // pipeline got Ctrl-Z); do that before going back to sleep.
    if (stoppending(p)) {
      release(&wait_lock);
      stopcheck();
      acquire(&wait_lock);
      continue;
    }

    // Wait for a child to exit or stop.
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep();
    acquire(&wait_lock);
  }
}

// Old-style wait(): any child, block, raw exit code.
int
kwait(uint64 addr)
{
  return kwaitpid(-1, addr, 0, 0);
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;) {
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting. Then turn them back off
    // to avoid a possible race between an interrupt
    // and wfi.
    intr_on();
    intr_off();

    int found = 0;
    for (p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if (p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Don't re-enable interrupts on release.
        mycpu()->intena = 0;

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
    }
    if (found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    first = 0;

    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Register current process as waiting for wakeups on chan.
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
}

// Wake up all processes sleeping on channel chan.
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // If the process is waiting for wakeups on this channel,
      // signal that the wakeup happened by clearing p->chan.
      p->chan = 0;

      // If this waiting process has gotten so far as to actually
      // go to sleep, also set it back to RUNNING.
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
      }
    }
    release(&p->lock);
  }
}

// Deliver signal sig to process p.  Caller must hold p->lock.
// Only default actions exist: terminate, stop, or continue.
// Safe to call from interrupt context (Ctrl-C / Ctrl-Z).
void
sigdeliver(struct proc *p, int sig)
{
  if (sig <= 0 || sig >= NSIG || p->state == UNUSED || p->state == ZOMBIE)
    return;

  if (sig == SIGKILL) {
    // cannot be ignored; even a stopped process must wake up to die.
    p->killed = SIGKILL;
    p->pendstop = 0;
    if (p->state == SLEEPING || p->state == STOPPED)
      p->state = RUNNABLE;
    return;
  }

  if (sig == SIGCONT) {
    // continue even if ignored; cancel any stop not yet acted on.
    p->pendstop = 0;
    if (p->state == STOPPED) {
      p->stopsig = 0;
      p->state = RUNNABLE;
    }
    return;
  }

  if (sig != SIGSTOP && (p->sigignore & (1 << sig)))
    return; // ignored

  if (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) {
    // remember it; the process stops itself in stopcheck(),
    // at a point where it holds no locks.
    if (p->state != STOPPED)
      p->pendstop = sig;
  } else {
    // default action of every other signal: terminate.
    // the process exits next time it heads back to user space.
    if (p->killed == 0)
      p->killed = sig;
  }

  // wake it from sleep() so it notices soon. A stopped process
  // stays stopped: it will see p->killed after SIGCONT.
  if (p->state == SLEEPING)
    p->state = RUNNABLE;
}

// Send sig to process pid (pid > 0) or to every process in
// process group -pid (pid < 0).  Never signals init.
// Returns 0 if at least one process was found, -1 otherwise.
int
ksignal(int pid, int sig)
{
  struct proc *p;
  int found = -1;

  if (pid == 0 || sig <= 0 || sig >= NSIG)
    return -1;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p != initproc && p->state != UNUSED &&
        ((pid > 0 && p->pid == pid) || (pid < 0 && p->pgid == -pid))) {
      sigdeliver(p, sig);
      found = 0;
    }
    release(&p->lock);
  }
  return found;
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  if (pid == 0)
    return -1;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED) {
      sigdeliver(p, SIGKILL);
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = SIGSEGV; // only used for bad traps
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Has p been asked to stop (and not killed)?
int
stoppending(struct proc *p)
{
  int s;

  acquire(&p->lock);
  s = p->pendstop && !p->killed;
  release(&p->lock);
  return s;
}

// If the current process has a pending stop signal, stop it here
// until SIGCONT (or SIGKILL) arrives.  Called only at safe points
// where the caller holds no locks: on the way back to user space,
// and inside a few interruptible waits (console, pipe, pause, wait).
void
stopcheck(void)
{
  struct proc *p = myproc();

  if (!stoppending(p))
    return;

  acquire(&wait_lock);

  // parent might be sleeping in waitpid(WUNTRACED).
  // (same order as kexit(): wake the parent first, then mark
  // ourselves; the parent can't look until we drop wait_lock.)
  wakeup(p->parent);

  acquire(&p->lock);
  if (p->pendstop && !p->killed) {
    p->stopsig = p->pendstop;
    p->pendstop = 0;
    p->state = STOPPED;
    release(&wait_lock);
    // give up the CPU; the scheduler ignores STOPPED processes,
    // so we sleep here until sigdeliver() makes us RUNNABLE.
    sched();
  } else {
    release(&wait_lock); // SIGCONT/SIGKILL got here first
  }
  release(&p->lock);
}

// Put process pid (0 = caller) into process group pgid
// (0 = a new group whose ID is pid).  Only the caller itself
// or one of its children may be moved.
int
ksetpgid(int pid, int pgid)
{
  struct proc *me = myproc();
  struct proc *p;
  int r = -1;

  if (pid < 0 || pgid < 0)
    return -1;
  if (pid == 0)
    pid = me->pid;
  if (pgid == 0)
    pgid = pid;

  acquire(&wait_lock); // protects p->parent
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED && p->state != ZOMBIE &&
        (p == me || p->parent == me)) {
      p->pgid = pgid;
      r = 0;
    }
    release(&p->lock);
  }
  release(&wait_lock);
  return r;
}

// Return the process group of process pid (0 = caller).
int
kgetpgid(int pid)
{
  struct proc *p;
  int r = -1;

  if (pid == 0)
    pid = myproc()->pid;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid && p->state != UNUSED)
      r = p->pgid;
    release(&p->lock);
  }
  return r;
}

// Set the caller's disposition for sig to SIG_DFL or SIG_IGN.
// Returns the old disposition. SIGKILL and SIGSTOP can't be ignored.
int
ksigdisp(int sig, int disp)
{
  struct proc *p = myproc();
  int old;

  if (sig <= 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
    return -1;
  if (disp != SIG_DFL && disp != SIG_IGN)
    return -1;

  acquire(&p->lock);
  old = (p->sigignore & (1 << sig)) ? SIG_IGN : SIG_DFL;
  if (disp == SIG_IGN)
    p->sigignore |= (1 << sig);
  else
    p->sigignore &= ~(1 << sig);
  release(&p->lock);
  return old;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    // clang-format off
    [UNUSED]    = "unused",
    [USED]      = "used",
    [SLEEPING]  = "sleep ",
    [RUNNABLE]  = "runble",
    [RUNNING]   = "run   ",
    [ZOMBIE]    = "zombie",
    [STOPPED]   = "stop  "
    // clang-format on
  };
  struct proc *p;
  char *state;

  printk("\npid pgid state name\n");
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printk("%d %d %s %s", p->pid, p->pgid, state, p->name);
    printk("\n");
  }
}
