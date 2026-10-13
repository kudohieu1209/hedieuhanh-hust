// ttydemo: show the three terminal modes set through ioctl().
//   ttydemo cooked   read whole lines (the kernel does the editing)
//   ttydemo noecho   read a password without showing it
//   ttydemo raw      show the code of every key at once; q quits

#include "kernel/types.h"
#include "kernel/termios.h"
#include "user/user.h"

struct termios saved; // mode to put back before we exit

void
setmode(int lflag)
{
  struct termios t;

  t.lflag = lflag;
  if (tcsetattr(0, &t) < 0) {
    fprintf(2, "ttydemo: stdin is not a terminal\n");
    exit(1);
  }
}

void
cooked(void)
{
  char buf[128];
  int n;

  setmode(TTY_COOKED);
  printf("cooked mode: type lines (try backspace, ^U, ^W), \"q\" to quit\n");
  while ((n = read(0, buf, sizeof(buf) - 1)) > 0) {
    buf[n] = 0;
    if (strcmp(buf, "q\n") == 0)
      break;
    printf("read() returned %d bytes: %s", n, buf);
  }
}

void
noecho(void)
{
  char buf[64];
  int n;

  setmode(TTY_NOECHO);
  printf("password: ");
  n = read(0, buf, sizeof(buf));
  printf("\nyou typed %d characters (not shown)\n", n > 0 ? n - 1 : 0);
}

void
raw(void)
{
  char c;

  setmode(TTY_RAW);
  printf("raw mode: every key is delivered at once, even ^C; q quits\n");
  while (read(0, &c, 1) == 1) {
    if (c == 'q')
      break;
    if (c >= ' ' && c < 0x7f)
      printf("key '%c' = %d\n", c, c);
    else
      printf("key ^%c = %d\n", (c + '@') & 0x7f, c);
  }
}

int
main(int argc, char *argv[])
{
  if (argc != 2) {
    fprintf(2, "usage: ttydemo cooked|noecho|raw\n");
    exit(1);
  }
  if (tcgetattr(0, &saved) < 0) {
    fprintf(2, "ttydemo: stdin is not a terminal\n");
    exit(1);
  }

  if (strcmp(argv[1], "cooked") == 0)
    cooked();
  else if (strcmp(argv[1], "noecho") == 0)
    noecho();
  else if (strcmp(argv[1], "raw") == 0)
    raw();
  else
    fprintf(2, "ttydemo: unknown mode %s\n", argv[1]);

  tcsetattr(0, &saved);
  exit(0);
}
