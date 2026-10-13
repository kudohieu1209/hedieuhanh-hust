// Signal numbers, shared by kernel and user programs.
// Numbers follow Linux so they are easy to recognise.
// xv6 only supports the *default* action of each signal (or ignoring it);
// there are no user-level signal handlers.

#define SIGHUP  1  // terminal gone (sent to orphaned stopped processes)
#define SIGINT  2  // Ctrl-C: terminate
#define SIGKILL 9  // terminate, cannot be ignored
#define SIGSEGV 11 // bad memory access: terminate
#define SIGTERM 15 // polite terminate request
#define SIGCONT 18 // continue a stopped process
#define SIGSTOP 19 // stop, cannot be ignored
#define SIGTSTP 20 // Ctrl-Z: stop
#define SIGTTIN 21 // background process tried to read the terminal: stop
#define SIGTTOU 22 // (reserved; background write is allowed in xv6)
#define NSIG    32

// dispositions for signal()
#define SIG_DFL 0 // default action
#define SIG_IGN 1 // ignore the signal
