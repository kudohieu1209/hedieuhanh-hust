//
// Console input and output, to the uart: the TTY layer.
//
// NTTY virtual terminals share the one UART. Minor device number n
// is terminal n. Only the "active" terminal is shown on the UART;
// the others keep their output in a buffer until the user switches
// to them with control-t. Each terminal has its own input buffer,
// mode flags (termios.h) and foreground process group.
//
// Special input characters in cooked mode (ICANON):
//   newline   -- end of line
//   control-h -- backspace (also DEL)
//   control-u -- kill line
//   control-w -- erase word
//   control-d -- end of file
// With ISIG:
//   control-c -- send SIGINT to the foreground process group
//   control-z -- send SIGTSTP to the foreground process group
// Always (handled by the kernel, never seen by programs):
//   control-t -- switch to the next virtual terminal
//   control-p -- print process list
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "signal.h"
#include "termios.h"

#define BACKSPACE 0x100       // erase the last output character
#define C(x)      ((x) - '@') // Control-x

//
// send one character to the uart, but don't use
// interrupts or sleep(). safe to be called from
// interrupts, e.g. by printk and to echo input
// characters.
//
void
consputc(int c)
{
  if (c == BACKSPACE) {
    // if the user typed backspace, overwrite with a space.
    uartputc_sync('\b');
    uartputc_sync(' ');
    uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

#define INPUT_BUF_SIZE  128
#define OUTPUT_BUF_SIZE 2048

struct tty {
  // input circular buffer
  char buf[INPUT_BUF_SIZE];
  uint r; // Read index
  uint w; // Write index: bytes before w may be read()
  uint e; // Edit index: bytes between w and e are still being edited

  int lflag;  // ECHO | ICANON | ISIG, see termios.h
  int fgpgid; // foreground process group, 0 = not set yet

  // output written while this terminal was not on screen
  char obuf[OUTPUT_BUF_SIZE];
  uint or; // first byte not shown yet
  uint ow; // where the next byte goes

  // the current (unfinished) screen line, e.g. the shell's prompt,
  // so it can be drawn again when the user switches back here.
  char line[INPUT_BUF_SIZE];
  int linelen;
};

struct {
  struct spinlock lock;
  struct tty tty[NTTY];
  int active; // terminal currently shown on the UART
} cons;

//
// user write() system calls to the console go here.
// uses sleep() and UART interrupts.
//
int
consolewrite(int minor, int user_src, uint64 src, int n)
{
  char buf[32]; // move batches from user space to uart.
  int i = 0;
  struct tty *t;

  if (minor < 0 || minor >= NTTY)
    return -1;
  t = &cons.tty[minor];

  while (i < n) {
    int nn = sizeof(buf);
    if (nn > n - i)
      nn = n - i;
    if (either_copyin(buf, user_src, src + i, nn) == -1)
      break;

    acquire(&cons.lock);
    for (int j = 0; j < nn; j++) {
      // a new line (or a carriage return) starts the line over.
      if (buf[j] == '\n' || buf[j] == '\r')
        t->linelen = 0;
      else if (t->linelen < INPUT_BUF_SIZE)
        t->line[t->linelen++] = buf[j];
    }
    if (minor != cons.active) {
      // not on screen: keep it until the user switches here.
      // if the buffer is full, the oldest output is lost.
      for (int j = 0; j < nn; j++) {
        t->obuf[t->ow++ % OUTPUT_BUF_SIZE] = buf[j];
        if (t->ow - t->or > OUTPUT_BUF_SIZE)
          t->or = t->ow - OUTPUT_BUF_SIZE;
      }
      release(&cons.lock);
    } else {
      release(&cons.lock);
      uartwrite(buf, nn);
    }
    i += nn;
  }

  return i;
}

//
// user read()s from the console go here.
// cooked mode: copy (up to) a whole input line to dst.
// raw mode: copy whatever has been typed, at least one byte.
// user_dst indicates whether dst is a user
// or kernel address.
//
int
consoleread(int minor, int user_dst, uint64 dst, int n)
{
  uint target;
  int c;
  char cbuf;
  struct tty *t;
  struct proc *p = myproc();

  if (minor < 0 || minor >= NTTY)
    return -1;
  t = &cons.tty[minor];

  target = n;
  acquire(&cons.lock);
  while (n > 0) {
    // job control: only the foreground process group may read
    // the terminal. A background reader sends SIGTTIN to its own
    // group, which stops the whole job (the shell then reports
    // "Stopped (tty input)"); it tries again after `fg`.
    if (t->fgpgid != 0 && p->pgid != t->fgpgid) {
      release(&cons.lock);
      if (n < target)
        return target - n;
      if (p->sigignore & (1 << SIGTTIN))
        return -1; // can't stop it, so fail the read instead
      ksignal(-p->pgid, SIGTTIN);
      stopcheck();
      if (killed(p))
        return -1;
      acquire(&cons.lock);
      continue;
    }

    // wait until interrupt handler has put some
    // input into the buffer.
    if (t->r == t->w) {
      if (n < target)
        break; // raw mode: return what we have so far
      if (killed(p)) {
        release(&cons.lock);
        return -1;
      }
      if (stoppending(p)) {
        // Ctrl-Z while waiting for input: stop here,
        // and keep waiting after SIGCONT.
        release(&cons.lock);
        stopcheck();
        acquire(&cons.lock);
        continue;
      }
      sleep_prepare(&t->r);
      release(&cons.lock);
      sleep();
      acquire(&cons.lock);
      continue;
    }

    c = t->buf[t->r++ % INPUT_BUF_SIZE];

    if ((t->lflag & ICANON) && c == C('D')) { // end-of-file
      if (n < target) {
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        t->r--;
      }
      break;
    }

    // copy the input byte to the user-space buffer.
    cbuf = c;
    if (either_copyout(user_dst, dst, &cbuf, 1) == -1)
      break;

    dst++;
    --n;

    if ((t->lflag & ICANON) && c == '\n') {
      // a whole line has arrived, return to
      // the user-level read().
      break;
    }
  }
  release(&cons.lock);

  return target - n;
}

//
// ioctl() on a terminal: get/set mode flags and
// the foreground process group.
//
int
consoleioctl(int minor, int req, uint64 arg)
{
  struct tty *t;
  struct termios tio;
  int pgid;

  if (minor < 0 || minor >= NTTY)
    return -1;
  t = &cons.tty[minor];

  switch (req) {
  case TCGETS:
    acquire(&cons.lock);
    tio.lflag = t->lflag;
    release(&cons.lock);
    return either_copyout(1, arg, &tio, sizeof(tio));

  case TCSETS:
    if (either_copyin(&tio, 1, arg, sizeof(tio)) < 0)
      return -1;
    acquire(&cons.lock);
    t->lflag = tio.lflag & (ECHO | ICANON | ISIG);
    if (!(t->lflag & ICANON)) {
      // leaving cooked mode: a half-typed line becomes readable.
      t->w = t->e;
      wakeup(&t->r);
    }
    release(&cons.lock);
    return 0;

  case TIOCGPGRP:
    acquire(&cons.lock);
    pgid = t->fgpgid;
    release(&cons.lock);
    return either_copyout(1, arg, &pgid, sizeof(pgid));

  case TIOCSPGRP:
    if (either_copyin(&pgid, 1, arg, sizeof(pgid)) < 0 || pgid <= 0)
      return -1;
    acquire(&cons.lock);
    t->fgpgid = pgid;
    // readers that were in the background may now be in front.
    wakeup(&t->r);
    release(&cons.lock);
    return 0;
  }
  return -1;
}

// erase the last character on the screen, if it was echoed.
static void
echoerase(struct tty *t)
{
  if (t->lflag & ECHO)
    consputc(BACKSPACE);
}

// show terminal n on the UART. caller holds cons.lock.
static void
switchtty(int n)
{
  struct tty *t = &cons.tty[n];
  char *s;

  cons.active = n;
  for (s = "\n[tty"; *s; s++)
    consputc(*s);
  consputc('0' + n);
  consputc(']');
  consputc('\n');

  // what this terminal printed while it was hidden; if nothing,
  // at least the line it was showing (e.g. the prompt).
  if (t->or == t->ow)
    for (int i = 0; i < t->linelen; i++)
      consputc(t->line[i]);
  while (t->or != t->ow)
    consputc(t->obuf[t->or++ % OUTPUT_BUF_SIZE]);

  // the line the user had started typing here (cooked mode).
  if ((t->lflag & (ICANON | ECHO)) == (ICANON | ECHO))
    for (uint i = t->w; i != t->e; i++)
      consputc(t->buf[i % INPUT_BUF_SIZE]);
}

//
// the console input interrupt handler.
// uartintr() calls this for each input character.
// runs in interrupt context: it must not sleep.
// do erase/kill processing, append to the input buffer,
// wake up consoleread() if a whole line has arrived,
// and turn Ctrl-C / Ctrl-Z into signals.
//
void
consoleintr(int c)
{
  struct tty *t;

  acquire(&cons.lock);
  t = &cons.tty[cons.active];

  if (c == C('T')) {
    // switch to the next virtual terminal.
    switchtty((cons.active + 1) % NTTY);
    release(&cons.lock);
    return;
  }

  if (c == C('P')) { // Print process list (a kernel debugging aid).
    procdump();
    release(&cons.lock);
    return;
  }

  if ((t->lflag & ISIG) && (c == C('C') || c == C('Z'))) {
    // throw away the half-typed line, show ^C or ^Z, and signal
    // every process in the foreground group. (we only mark the
    // processes here; each one acts on the signal itself.)
    t->e = t->w;
    if (t->lflag & ECHO) {
      consputc('^');
      consputc(c + '@');
      consputc('\n');
    }
    if (t->fgpgid > 0)
      ksignal(-t->fgpgid, c == C('C') ? SIGINT : SIGTSTP);
    release(&cons.lock);
    return;
  }


  if (!(t->lflag & ICANON)) {
    // raw mode: no line editing; every byte can be read at once.
    if (c != 0 && t->e - t->r < INPUT_BUF_SIZE) {
      if (t->lflag & ECHO)
        consputc(c);
      t->buf[t->e++ % INPUT_BUF_SIZE] = c;
      t->w = t->e;
      wakeup(&t->r);
    }
    release(&cons.lock);
    return;
  }

  switch (c) {
  case C('U'): // Kill line.
    while (t->e != t->w &&
           t->buf[(t->e - 1) % INPUT_BUF_SIZE] != '\n') {
      t->e--;
      echoerase(t);
    }
    break;
  case C('W'): // Erase word: spaces, then the word before them.
    while (t->e != t->w && t->buf[(t->e - 1) % INPUT_BUF_SIZE] == ' ') {
      t->e--;
      echoerase(t);
    }
    while (t->e != t->w && t->buf[(t->e - 1) % INPUT_BUF_SIZE] != ' ') {
      t->e--;
      echoerase(t);
    }
    break;
  case C('H'): // Backspace
  case '\x7f': // Delete key
    if (t->e != t->w) {
      t->e--;
      echoerase(t);
    }
    break;
  default:
    if (c != 0 && t->e - t->r < INPUT_BUF_SIZE) {
      c = (c == '\r') ? '\n' : c;

      // echo back to the user.
      if (t->lflag & ECHO)
        consputc(c);

      // store for consumption by consoleread().
      t->buf[t->e++ % INPUT_BUF_SIZE] = c;

      if (c == '\n' || c == C('D') || t->e - t->r == INPUT_BUF_SIZE) {
        // wake up consoleread() if a whole line (or end-of-file)
        // has arrived.
        t->w = t->e;
        wakeup(&t->r);
      }
    }
    break;
  }

  release(&cons.lock);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");

  for (int i = 0; i < NTTY; i++)
    cons.tty[i].lflag = TTY_COOKED;
  cons.active = 0;

  uartinit();

  // connect read, write and ioctl system calls
  // to consoleread, consolewrite and consoleioctl.
  devsw[CONSOLE].read = consoleread;
  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].ioctl = consoleioctl;
}
