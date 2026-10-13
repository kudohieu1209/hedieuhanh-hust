// counter [n]: print a number about once a second, n times
// (default: forever). Handy for testing Ctrl-Z, bg, fg and Ctrl-C.

#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int n = argc > 1 ? atoi(argv[1]) : 0;

  for (int i = 1; n == 0 || i <= n; i++) {
    printf("counter[%d]: %d\n", getpid(), i);
    pause(10); // 10 ticks = about 1 second
  }
  exit(0);
}
