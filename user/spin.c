// spin: use the CPU without ever sleeping, printing a dot now and then.
// Unlike counter, it is stopped by Ctrl-Z through the timer interrupt
// path, since it is (almost) never inside a system call.

#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  volatile unsigned long x = 0;

  for (;;) {
    for (int i = 0; i < 100000000; i++)
      x += i;
    write(1, ".", 1);
  }
}
