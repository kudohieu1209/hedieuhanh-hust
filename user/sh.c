// Shell, with job control (jobs, fg, bg), a line editor and history.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/signal.h"
#include "kernel/wait.h"
#include "kernel/termios.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void); // Fork but panics on failure.
void panic(char *);
struct cmd *parsecmd(char *);
void runcmd(struct cmd *) __attribute__((noreturn));

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if (cmd == 0)
    exit(1);

  switch (cmd->type) {
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd *)cmd;
    if (ecmd->argv[0] == 0)
      exit(1);
    exec(ecmd->argv[0], ecmd->argv);
    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

  case REDIR:
    rcmd = (struct redircmd *)cmd;
    close(rcmd->fd);
    if (open(rcmd->file, rcmd->mode) < 0) {
      fprintf(2, "open %s failed\n", rcmd->file);
      exit(1);
    }
    runcmd(rcmd->cmd);
    break;

  case LIST:
    lcmd = (struct listcmd *)cmd;
    if (fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd *)cmd;
    if (pipe(p) < 0)
      panic("pipe");
    if (fork1() == 0) {
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if (fork1() == 0) {
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd *)cmd;
    if (fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

//PAGEBREAK!
// Job control
//
// Every command line the shell runs is a "job". The shell forks one
// process for it (the job leader) and puts it in a new process group
// whose ID is the leader's pid; any processes the leader forks (e.g.
// the two sides of a pipe) inherit that group. Ctrl-C and Ctrl-Z are
// sent by the kernel to the terminal's foreground group, so only the
// foreground job gets them, never the shell or background jobs.

#define MAXJOBS 16

enum jobstate { JOB_FREE, JOB_RUNNING, JOB_STOPPED, JOB_DONE };

struct job {
  enum jobstate state;
  int id;                // job number, shown as [id]
  int pgid;              // process group (= pid of the job leader)
  int status;            // last waitpid() status
  int notify;            // state changed; tell the user before the next prompt
  int hasmodes;          // tmodes is valid
  struct termios tmodes; // terminal modes the job had when it stopped
  char cmd[100];         // command text, for `jobs`
} jobs[MAXJOBS];

int interactive;             // stdin is a terminal: do job control
int shell_pgid;              // the shell's own process group
struct termios shell_tmodes; // modes the shell gives to new jobs
int curjob;                  // default job for fg/bg (most recent)

struct job *
findjob(int id)
{
  for (int i = 0; i < MAXJOBS; i++)
    if (jobs[i].state != JOB_FREE && jobs[i].id == id)
      return &jobs[i];
  return 0;
}

struct job *
jobbypgid(int pgid)
{
  for (int i = 0; i < MAXJOBS; i++)
    if (jobs[i].state != JOB_FREE && jobs[i].pgid == pgid)
      return &jobs[i];
  return 0;
}

struct job *
addjob(int pgid, char *cmd)
{
  int id = 1;

  for (int i = 0; i < MAXJOBS; i++)
    if (jobs[i].state != JOB_FREE && jobs[i].id >= id)
      id = jobs[i].id + 1;
  for (int i = 0; i < MAXJOBS; i++) {
    struct job *j = &jobs[i];
    if (j->state == JOB_FREE) {
      memset(j, 0, sizeof(*j));
      j->state = JOB_RUNNING;
      j->id = id;
      j->pgid = pgid;
      strcpy(j->cmd, cmd);
      curjob = id;
      return j;
    }
  }
  return 0;
}

void
printjob(struct job *j)
{
  char *s;

  if (j->state == JOB_RUNNING)
    s = "Running";
  else if (j->state == JOB_STOPPED)
    s = WSTOPSIG(j->status) == SIGTTIN ? "Stopped (tty input)" : "Stopped";
  else if (WIFSIGNALED(j->status))
    s = WTERMSIG(j->status) == SIGINT ? "Interrupted" : "Killed";
  else
    s = "Done";
  printf("[%d]%c %s\t%s%s\n", j->id, j->id == curjob ? '+' : ' ', s,
         j->cmd, j->state == JOB_RUNNING ? " &" : "");
}

// record a status returned by waitpid() for job j.
void
setstatus(struct job *j, int status)
{
  j->status = status;
  j->notify = 1;
  if (WIFSTOPPED(status)) {
    j->state = JOB_STOPPED;
    curjob = j->id;
  } else {
    j->state = JOB_DONE;
  }
}

// collect state changes of background jobs, without blocking.
void
updatejobs(void)
{
  int pid, status;
  struct job *j;

  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0)
    if ((j = jobbypgid(pid)) != 0)
      setstatus(j, status);
}

// print jobs (all, or only those whose state changed),
// then forget the ones that are finished.
void
listjobs(int all)
{
  struct job *j;

  updatejobs();
  for (int i = 0; i < MAXJOBS; i++) {
    j = &jobs[i];
    if (j->state != JOB_FREE && (all || j->notify))
      printjob(j);
    j->notify = 0;
    if (j->state == JOB_DONE)
      j->state = JOB_FREE;
  }
}

// give the terminal to job j and wait until it finishes or stops.
// if cont, it was stopped: wake it with SIGCONT.
void
fgjob(struct job *j, int cont)
{
  int status;

  if (j->hasmodes)
    tcsetattr(0, &j->tmodes); // modes the job had when it stopped
  tcsetpgrp(0, j->pgid);
  if (cont)
    sigsend(-j->pgid, SIGCONT);
  j->state = JOB_RUNNING;

  // WUNTRACED: also return when the job stops (Ctrl-Z),
  // not only when it exits.
  if (waitpid(j->pgid, &status, WUNTRACED) < 0)
    status = 0;
  setstatus(j, status);

  // take the terminal back, remembering the job's modes.
  tcsetpgrp(0, shell_pgid);
  if (j->state == JOB_STOPPED) {
    tcgetattr(0, &j->tmodes);
    j->hasmodes = 1;
    printjob(j);
  } else {
    if (WIFSIGNALED(status) && WTERMSIG(status) != SIGINT)
      printjob(j); // e.g. "Killed"
    j->state = JOB_FREE;
  }
  j->notify = 0;
  tcsetattr(0, &shell_tmodes);
}

// run one command line as a new job.
void
runjob(char *line)
{
  char text[100];
  int bg = 0, pid;
  char *e;
  struct job *j;

  // a trailing '&' means: run in the background.
  e = line + strlen(line);
  while (e > line && strchr(" \t\r\n", e[-1]))
    e--;
  if (e > line && e[-1] == '&') {
    bg = 1;
    e[-1] = ' ';
    while (e > line && strchr(" \t\r\n&", e[-1]))
      e--;
  }
  memmove(text, line, e - line);
  text[e - line] = 0;

  pid = fork1();
  if (pid == 0) {
    if (interactive) {
      // own process group; take the terminal if in the foreground.
      // (the parent does the same; whoever runs first wins the race.)
      setpgid(0, 0);
      if (!bg)
        tcsetpgrp(0, getpid());
      // the shell ignores these, but the job must not.
      signal(SIGINT, SIG_DFL);
      signal(SIGTSTP, SIG_DFL);
      signal(SIGTTIN, SIG_DFL);
      signal(SIGTTOU, SIG_DFL);
    }
    runcmd(parsecmd(line));
  }

  if (!interactive) {
    if (!bg)
      waitpid(pid, 0, 0);
    return;
  }

  setpgid(pid, pid);
  if ((j = addjob(pid, text)) == 0) {
    fprintf(2, "sh: too many jobs\n");
    sigsend(-pid, SIGKILL);
    waitpid(pid, 0, 0);
    return;
  }
  if (bg)
    printf("[%d] %d\n", j->id, pid);
  else
    fgjob(j, 0);
}

// "%2", "2" or nothing (= current job)
struct job *
jobarg(char *arg)
{
  struct job *j;

  if (arg == 0)
    j = findjob(curjob);
  else
    j = findjob(atoi(arg[0] == '%' ? arg + 1 : arg));
  if (j == 0)
    fprintf(2, "sh: no such job\n");
  return j;
}

//PAGEBREAK!
// Line editor with history.
//
// While the shell waits for a command it puts the terminal in raw
// mode, so it sees every key at once and does the editing itself:
// left/right arrows, Home/End (^A/^E), backspace, ^U, and up/down
// to walk through earlier commands. Programs it runs get the normal
// cooked mode back.

#define NHIST 16
#define C(x)  ((x) - '@') // Control-x

char history[NHIST][100];
int nhist; // number of commands ever added

void
addhistory(char *cmd)
{
  int n = strlen(cmd);

  while (n > 0 && cmd[n - 1] == '\n')
    n--;
  if (n == 0 || n >= sizeof(history[0]))
    return;
  if (nhist > 0) {
    char *last = history[(nhist - 1) % NHIST];
    if (strlen(last) == n && memcmp(last, cmd, n) == 0)
      return; // same as the previous command
  }
  memmove(history[nhist % NHIST], cmd, n);
  history[nhist % NHIST][n] = 0;
  nhist++;
}

// redraw the prompt and line, then put the cursor at pos.
void
redraw(char *buf, int len, int pos)
{
  write(2, "\r$ ", 3);
  write(2, buf, len);
  write(2, "\x1b[K", 3); // erase the rest of the screen line
  if (len > pos)
    fprintf(2, "\x1b[%dD", len - pos); // cursor left
}

// read one line in raw mode. returns its length, or -1 at end of file.
int
readline(char *buf, int nbuf)
{
  struct termios raw;
  int len = 0, pos = 0, h = nhist;
  char c, seq[2];

  raw.lflag = TTY_RAW;
  tcsetattr(0, &raw);
  write(2, "$ ", 2);

  for (;;) {
    if (read(0, &c, 1) != 1) {
      len = -1;
      break;
    }
    if (c == '\r' || c == '\n') {
      write(2, "\n", 1);
      break;
    } else if (c == C('D') && len == 0) {
      len = -1; // EOF
      break;
    } else if (c == C('C')) {
      write(2, "^C\n", 3); // forget this line
      len = 0;
      break;
    } else if (c == 0x7f || c == C('H')) {
      if (pos > 0) {
        memmove(buf + pos - 1, buf + pos, len - pos);
        pos--;
        len--;
      }
    } else if (c == C('U')) {
      len = pos = 0;
    } else if (c == C('A')) {
      pos = 0;
    } else if (c == C('E')) {
      pos = len;
    } else if (c == 0x1b) {
      // escape sequence sent by arrow keys: ESC [ A..D, H, F
      if (read(0, &seq[0], 1) != 1 || read(0, &seq[1], 1) != 1)
        continue;
      if (seq[0] != '[')
        continue;
      if (seq[1] == 'A' && h > 0 && h > nhist - NHIST) {
        h--; // older command
        len = pos = strlen(history[h % NHIST]);
        memmove(buf, history[h % NHIST], len);
      } else if (seq[1] == 'B' && h < nhist) {
        h++; // newer command, or an empty line after the newest
        len = pos = h < nhist ? strlen(history[h % NHIST]) : 0;
        memmove(buf, history[h % NHIST], len);
      } else if (seq[1] == 'C' && pos < len) {
        pos++;
      } else if (seq[1] == 'D' && pos > 0) {
        pos--;
      } else if (seq[1] == 'H') {
        pos = 0;
      } else if (seq[1] == 'F') {
        pos = len;
      }
    } else if (c >= ' ' && c < 0x7f && len < nbuf - 2) {
      memmove(buf + pos + 1, buf + pos, len - pos);
      buf[pos++] = c;
      len++;
    } else {
      continue;
    }
    redraw(buf, len, pos);
  }

  tcsetattr(0, &shell_tmodes);
  if (len >= 0) {
    buf[len] = '\n';
    buf[len + 1] = 0;
  }
  return len;
}

int
getcmd(char *buf, int nbuf)
{
  if (interactive) {
    listjobs(0); // e.g. "[1]  Done  counter 3"
    tcsetpgrp(0, shell_pgid); // in case a job took the terminal
    memset(buf, 0, nbuf);
    return readline(buf, nbuf) < 0 ? -1 : 0;
  }
  write(2, "$ ", 2);
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if (buf[0] == 0) // EOF
    return -1;
  return 0;
}

// built-in commands that must run inside the shell itself.
// returns 1 if cmd was one of them.
int
builtin(char *cmd)
{
  char tmp[100], *argv[3];
  int argc = 0;
  struct job *j;
  char *s;

  // split a copy into at most 3 words
  strcpy(tmp, cmd);
  s = tmp;
  while (argc < 3) {
    while (*s && strchr(" \t\r\n", *s))
      *s++ = 0;
    if (*s == 0)
      break;
    argv[argc++] = s;
    while (*s && !strchr(" \t\r\n", *s))
      s++;
  }
  *s = 0;
  if (argc == 0)
    return 0;

  if (strcmp(argv[0], "cd") == 0) {
    // Chdir must be called by the parent, not the child.
    if (argc < 2 || chdir(argv[1]) < 0)
      fprintf(2, "cannot cd %s\n", argc < 2 ? "" : argv[1]);
  } else if (strcmp(argv[0], "exit") == 0) {
    exit(0);
  } else if (!interactive) {
    return 0;
  } else if (strcmp(argv[0], "jobs") == 0) {
    listjobs(1);
  } else if (strcmp(argv[0], "fg") == 0) {
    if ((j = jobarg(argc > 1 ? argv[1] : 0)) != 0) {
      printf("%s\n", j->cmd);
      curjob = j->id;
      fgjob(j, 1);
    }
  } else if (strcmp(argv[0], "bg") == 0) {
    if ((j = jobarg(argc > 1 ? argv[1] : 0)) != 0) {
      j->state = JOB_RUNNING;
      curjob = j->id;
      sigsend(-j->pgid, SIGCONT);
      printjob(j);
    }
  } else if (strcmp(argv[0], "kill") == 0 && argc > 1 && argv[1][0] == '%') {
    // kill %n: terminate a whole job, even a stopped one
    if ((j = jobarg(argv[1])) != 0)
      sigsend(-j->pgid, SIGKILL);
  } else if (strcmp(argv[0], "history") == 0) {
    for (int i = nhist > NHIST ? nhist - NHIST : 0; i < nhist; i++)
      printf("%d  %s\n", i + 1, history[i % NHIST]);
  } else {
    return 0;
  }
  return 1;
}

int
main(void)
{
  static char buf[100];
  int fd;

  // Ensure that three file descriptors are open.
  while ((fd = open("console", O_RDWR)) >= 0) {
    if (fd >= 3) {
      close(fd);
      break;
    }
  }

  interactive = isatty(0);
  if (interactive) {
    // put the shell in its own process group and take the terminal.
    setpgid(0, 0);
    shell_pgid = getpid();
    tcsetpgrp(0, shell_pgid);
    // keyboard signals are for jobs, not for the shell.
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    shell_tmodes.lflag = TTY_COOKED;
    tcsetattr(0, &shell_tmodes);
  }

  // Read and run input commands.
  while (getcmd(buf, sizeof(buf)) >= 0) {
    char *cmd = buf;
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    if (*cmd == '\n' || *cmd == 0) // is a blank command
      continue;
    if (interactive)
      addhistory(cmd);
    if (!builtin(cmd))
      runjob(cmd);
  }
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if (pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd *
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd *)cmd;
}

struct cmd *
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd *)cmd;
}

struct cmd *
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd *)cmd;
}

struct cmd *
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd *)cmd;
}

struct cmd *
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd *)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while (s < es && strchr(whitespace, *s))
    s++;
  if (q)
    *q = s;
  ret = *s;
  switch (*s) {
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if (*s == '>') {
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if (eq)
    *eq = s;

  while (s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while (s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);
struct cmd *parsepipe(char **, char *);
struct cmd *parseexec(char **, char *);
struct cmd *nulterminate(struct cmd *);

struct cmd *
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if (s != es) {
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd *
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while (peek(ps, es, "&")) {
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if (peek(ps, es, ";")) {
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd *
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if (peek(ps, es, "|")) {
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd *
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while (peek(ps, es, "<>")) {
    tok = gettoken(ps, es, 0, 0);
    if (gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch (tok) {
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE | O_TRUNC, 1);
      break;
    case '+': // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd *
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if (!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if (!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd *
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if (peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd *)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while (!peek(ps, es, "|)&;")) {
    if ((tok = gettoken(ps, es, &q, &eq)) == 0)
      break;
    if (tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if (argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd *
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if (cmd == 0)
    return 0;

  switch (cmd->type) {
  case EXEC:
    ecmd = (struct execcmd *)cmd;
    for (i = 0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd *)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd *)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd *)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd *)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}
