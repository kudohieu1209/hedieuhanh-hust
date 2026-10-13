// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "kernel/param.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = {"sh", 0};

// one shell per virtual terminal; Ctrl-T switches between them.
// "console" is terminal 0, "tty1" is terminal 1 (same device,
// different minor number).
char *ttyname[NTTY] = {"console", "tty1"};
int shpid[NTTY];

// open terminal i as stdin, stdout and stderr of a new shell.
void
startsh(int i)
{
  int pid;

  printf("init: starting sh on %s\n", ttyname[i]);
  pid = fork();
  if (pid < 0) {
    printf("init: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    if (i != 0) {
      close(0);
      close(1);
      close(2);
      if (open(ttyname[i], O_RDWR) < 0) {
        mknod(ttyname[i], CONSOLE, i);
        open(ttyname[i], O_RDWR);
      }
      dup(0); // stdout
      dup(0); // stderr
    }
    exec("sh", argv);
    printf("init: exec sh failed\n");
    exit(1);
  }
  shpid[i] = pid;
}

int
main(void)
{
  int wpid;

  if (open("console", O_RDWR) < 0) {
    mknod("console", CONSOLE, 0);
    open("console", O_RDWR);
  }
  dup(0); // stdout
  dup(0); // stderr

  for (int i = 0; i < NTTY; i++)
    startsh(i);

  for (;;) {
    // this call to wait() returns if a shell exits,
    // or if a parentless process exits.
    wpid = wait((int *)0);
    if (wpid < 0) {
      printf("init: wait returned an error\n");
      exit(1);
    }
    for (int i = 0; i < NTTY; i++)
      if (wpid == shpid[i])
        startsh(i); // the shell exited; restart it.
  }
}
