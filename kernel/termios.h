// Terminal settings and ioctl() requests, shared by kernel and user.

struct termios {
  int lflag; // local mode flags, see below
};

// lflag bits
#define ECHO   0x1 // echo typed characters back to the screen
#define ICANON 0x2 // canonical ("cooked") mode: line editing, read() returns whole lines
#define ISIG   0x4 // Ctrl-C / Ctrl-Z generate SIGINT / SIGTSTP

// common modes
#define TTY_COOKED (ECHO | ICANON | ISIG)
#define TTY_NOECHO (ICANON | ISIG)
#define TTY_RAW    0

// ioctl() requests for terminal devices
#define TCGETS    1 // arg: struct termios *, read settings
#define TCSETS    2 // arg: struct termios *, change settings
#define TIOCGPGRP 3 // arg: int *, read foreground process group
#define TIOCSPGRP 4 // arg: int *, set foreground process group
