#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/signal.h"
#include "user/user.h"

// kill pid...          kill processes (SIGKILL, like the original xv6 kill)
// kill -SIG pid...     send SIG (a number or INT, KILL, TERM, CONT, STOP,
//                      TSTP); a negative pid means a whole process group.

struct {
  char *name;
  int sig;
} signames[] = {
  {"HUP", SIGHUP},   {"INT", SIGINT},   {"KILL", SIGKILL},
  {"TERM", SIGTERM}, {"CONT", SIGCONT}, {"STOP", SIGSTOP},
  {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
};

int
signum(char *s)
{
  if (*s >= '0' && *s <= '9')
    return atoi(s);
  for (int i = 0; i < sizeof(signames) / sizeof(signames[0]); i++)
    if (strcmp(s, signames[i].name) == 0)
      return signames[i].sig;
  return -1;
}

int
main(int argc, char **argv)
{
  int i = 1, sig = 0;

  if (argc > 2 && argv[1][0] == '-') {
    if ((sig = signum(argv[1] + 1)) <= 0) {
      fprintf(2, "kill: unknown signal %s\n", argv[1] + 1);
      exit(1);
    }
    i = 2;
  }
  if (i >= argc) {
    fprintf(2, "usage: kill [-SIG] pid...  (negative pid = process group)\n");
    exit(1);
  }
  for (; i < argc; i++) {
    char *s = argv[i];
    int neg = (*s == '-');
    int pid = atoi(neg ? s + 1 : s);
    if (neg)
      pid = -pid;
    if ((sig ? sigsend(pid, sig) : kill(pid)) < 0)
      fprintf(2, "kill: %s: no such process\n", s);
  }
  exit(0);
}
