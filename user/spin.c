// spin: use the CPU without ever sleeping, printing a dot now and then.
// Unlike counter, it is stopped by Ctrl-Z through the timer interrupt
// path, since it is (almost) never inside a system call.

#include "kernel/types.h"
#include "user/user.h"

// global, so newer GCCs don't flag it as "set but not used" (-Werror on macOS CI).
volatile unsigned long spin_sum;

int
main(int argc, char *argv[])
{
  for (;;) {
    for (int i = 0; i < 100000000; i++)
      spin_sum += i;
    write(1, ".", 1);
  }
}
