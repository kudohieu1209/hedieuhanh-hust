// waitpid() options and status decoding, shared by kernel and user.
// The status word uses the same layout as Linux:
//   exited normally:   (exit code << 8)
//   killed by signal:  signal number (1..126)
//   stopped:           (stop signal << 8) | 0x7f

#define WNOHANG   1 // don't block if no child has changed state
#define WUNTRACED 2 // also report children that have stopped

#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)    ((s) & 0x7f)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)    (((s) >> 8) & 0xff)
