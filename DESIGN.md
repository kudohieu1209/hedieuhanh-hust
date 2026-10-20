# Thiết kế: TTY và Job Control trên xv6 (Đề #27)

Bản xv6 gốc: `mit-pdos/xv6-riscv` commit `06aad25`. Mọi thay đổi nằm trên nhánh `tty-jobctl`.

## 1. Bức tranh tổng thể

Ví dụ đời thường: **quầy lễ tân có 1 chiếc điện thoại (terminal)**. Nhiều nhóm khách (process group) cùng chờ,
nhưng tại mỗi thời điểm chỉ **một nhóm đứng trước quầy (foreground group)** được nghe/nói qua điện thoại.
Lễ tân (shell) quyết định nhóm nào được lên quầy. Khi khách bấm Ctrl-C/Ctrl-Z, tổng đài (kernel)
báo cho **cả nhóm đang đứng trước quầy**, không báo cho lễ tân hay các nhóm đang ngồi chờ.

```
 bàn phím ──UART──► consoleintr()  (ngữ cảnh ngắt, KHÔNG được ngủ)
                      │  Ctrl-T: đổi terminal ảo      Ctrl-P: procdump
                      │  Ctrl-C/Ctrl-Z (nếu ISIG): ksignal(-fgpgid, SIGINT/SIGTSTP)
                      │  cooked (ICANON): sửa dòng, chỉ đánh thức khi có '\n'
                      │  raw: mọi byte đọc được ngay
                      ▼
               struct tty[NTTY] ── buf, r/w/e, lflag, fgpgid, obuf (output khi bị ẩn)
                      ▲
 read()/write()/ioctl() ── consoleread / consolewrite / consoleioctl (minor = số terminal)
                      ▲
               user: sh (bảng job, line editor), ttydemo, counter, spin, kill
```

Chia làm 3 tầng:

| Tầng | File | Vai trò |
| :--- | :--- | :--- |
| Tiến trình + tín hiệu | `kernel/proc.c`, `proc.h`, `trap.c`, `pipe.c`, `sysproc.c`, `signal.h`, `wait.h` | nhóm tiến trình, trạng thái STOPPED, gửi tín hiệu, `waitpid` |
| TTY | `kernel/console.c`, `file.c/h`, `sysfile.c`, `termios.h` | chế độ Cooked/Raw/No-Echo, foreground group, Ctrl-C/Z, terminal ảo |
| User | `user/sh.c`, `init.c`, `ulib.c`, `printf.c`, `kill.c`, `counter.c`, `spin.c`, `ttydemo.c` | shell có job control, line editor + lịch sử, chương trình demo |

## 2. Thay đổi trong `struct proc`

```c
enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE, STOPPED };

int killed;     // (đổi nghĩa) 0, hoặc SỐ HIỆU tín hiệu đã giết tiến trình
int pgid;       // nhóm tiến trình
int pendstop;   // tín hiệu dừng đã nhận nhưng chưa dừng (0 = không)
int stopsig;    // đã dừng vì tín hiệu này, cha CHƯA được báo (0 = đã báo/không dừng)
uint sigignore; // bitmask các tín hiệu bị bỏ qua
```

- `fork()`: con **thừa hưởng** `pgid` và `sigignore` của cha.
- `exec()`: giữ nguyên cả hai (giống POSIX). Vì thế shell phải tự đặt lại `SIG_DFL` trong tiến trình con trước khi `exec`.

## 3. Tín hiệu thu nhỏ

Chỉ có **hành động mặc định** (không có handler ở user), đủ cho job control:

| Tín hiệu | Số | Hành động | Ai gửi |
| :--- | :-: | :--- | :--- |
| SIGINT | 2 | kết thúc | Ctrl-C |
| SIGKILL | 9 | kết thúc, không bỏ qua được, đánh thức cả tiến trình đang dừng | `kill pid`, `kill %n` |
| SIGTERM, SIGSEGV, SIGHUP | 15, 11, 1 | kết thúc | `kill -TERM`, lỗi trang, mồ côi bị dừng |
| SIGTSTP | 20 | dừng | Ctrl-Z |
| SIGTTIN | 21 | dừng | tiến trình nền đọc terminal |
| SIGSTOP | 19 | dừng, không bỏ qua được | `kill -STOP` |
| SIGCONT | 18 | chạy tiếp, hủy lệnh dừng đang chờ | `fg`, `bg` |

**Nguyên tắc quan trọng nhất: chỉ "đánh dấu", để tiến trình tự hành động.**
`sigdeliver()` (gọi được cả trong ngắt) chỉ đặt `p->killed` hoặc `p->pendstop`,
và nếu tiến trình đang `SLEEPING` thì chuyển sang `RUNNABLE` để nó sớm nhận ra.
Tiến trình **tự** chết (`kexit`) hoặc **tự** dừng (`stopcheck()`) tại một **điểm an toàn**,
là lúc nó không giữ khóa nào:

1. ngay trước khi quay về user space (`usertrap()`), áp dụng cho chương trình chiếm CPU như `spin`, dừng nhờ ngắt timer;
2. trong các vòng chờ có thể bị ngắt: `consoleread`, `piperead/pipewrite`, `pause` (`sys_pause`), `kwaitpid`.

Vì sao không dừng ngay trong `sleep()`? Vì có những chỗ ngủ khi **đang giữ sleeplock**,
ví dụ khóa inode hoặc log của file system. Dừng ở đó thì cả hệ thống file bị treo theo.

### Sơ đồ trạng thái

```
             fork                 scheduler
  UNUSED ──────────► RUNNABLE ◄──────────────► RUNNING ──── exit ───► ZOMBIE ── wait ──► UNUSED
                        ▲  ▲                     │  │
               wakeup/  │  │ SIGCONT, SIGKILL    │  │ stopcheck() thấy pendstop
               signal   │  └──────────── STOPPED ◄──┘ (Ctrl-Z, SIGTTIN, SIGSTOP)
                        │                            
                     SLEEPING ◄── sleep() ─────┘
```

`scheduler()` chỉ chọn tiến trình `RUNNABLE`, nên tiến trình `STOPPED` không bao giờ được chạy
cho tới khi `SIGCONT` hoặc `SIGKILL` chuyển nó về `RUNNABLE`.

### `stopcheck()`: dừng mà không mất tín hiệu

```
acquire(wait_lock)
wakeup(p->parent)          // báo cha (đang waitpid) TRƯỚC, giống kexit()
acquire(p->lock)
if (vẫn còn pendstop):     // SIGCONT có thể đã tới và hủy rồi
    stopsig = pendstop; state = STOPPED
    release(wait_lock)
    sched()                // nhường CPU, "ngủ" tới khi bị SIGCONT/SIGKILL
release(p->lock)
```

Cha không bỏ lỡ được: muốn quét danh sách con, cha phải lấy `wait_lock`,
mà ta chỉ nhả `wait_lock` sau khi đã đặt `STOPPED`.

### `waitpid(pid, &status, options)`

- `pid == -1`: con bất kỳ; `pid > 0`: đúng con đó; `pid < -1`: con bất kỳ trong nhóm `-pid`.
- `WUNTRACED`: báo cả con **bị dừng**. Mỗi lần dừng chỉ báo 1 lần (xóa `stopsig`).
- `WNOHANG`: không chặn; trả 0 nếu chưa con nào đổi trạng thái.
- Mã `status` giống Linux, đọc bằng `WIFEXITED/WIFSIGNALED/WIFSTOPPED/WSTOPSIG...` (`kernel/wait.h`).
- `wait()` cũ giữ nguyên hành vi (trả mã exit thô, `-1` nếu bị kill), nên `usertests` không bị ảnh hưởng.

### Tiến trình mồ côi đang dừng

Khi cha chết, con được giao cho `init`. Nếu con đang `STOPPED` thì không ai còn `fg`/`bg` nó được nữa,
nên `reparent()` gửi `SIGHUP` (để nó kết thúc) **rồi** `SIGCONT` (để nó được chạy mà nhận ra mình bị kết thúc), giống Unix.
Nếu chỉ gửi SIGHUP thì tiến trình vẫn đứng yên ở `STOPPED` mãi mãi.

## 4. Lớp TTY (`kernel/console.c`)

```c
struct tty {
  char buf[128]; uint r, w, e;   // r: đã đọc tới, w: đọc được tới, e: đang sửa tới
  int lflag;                     // ECHO | ICANON | ISIG
  int fgpgid;                    // nhóm tiền cảnh, 0 = chưa đặt
  char obuf[2048]; uint or, ow;  // output khi terminal đang bị ẩn
  char line[128]; int linelen;   // dòng hiện tại trên màn hình (để vẽ lại prompt)
};
```

### Ba chế độ, đặt bằng `ioctl(fd, TCSETS, &termios)`

| Chế độ | `lflag` | `read()` trả về khi | Sửa dòng | Echo | Ctrl-C/Z |
| :--- | :--- | :--- | :-: | :-: | :-: |
| Cooked | `ECHO\|ICANON\|ISIG` | có cả dòng (`\n`) hoặc Ctrl-D | kernel làm | có | thành tín hiệu |
| No-Echo | `ICANON\|ISIG` | có cả dòng | kernel làm | **không** | thành tín hiệu |
| Raw | `0` | có **ít nhất 1 byte** | chương trình tự làm | không | chỉ là byte 3 / 26 |

Các request `ioctl`: `TCGETS`, `TCSETS`, `TIOCGPGRP`, `TIOCSPGRP` (`kernel/termios.h`).
Phía user có các hàm bọc `tcgetattr`, `tcsetattr`, `tcgetpgrp`, `tcsetpgrp`, `isatty` (`user/ulib.c`).
`ioctl` đi qua bảng `devsw` như `read`/`write`: `devsw` có thêm con trỏ `ioctl`,
và mọi hàm của thiết bị nhận thêm `minor` (số hiệu terminal).

### Phím đặc biệt

| Phím | Khi nào | Tác dụng |
| :--- | :--- | :--- |
| Backspace/DEL, Ctrl-U, **Ctrl-W** | cooked | xóa ký tự / cả dòng / một từ |
| Ctrl-D | cooked | EOF |
| Ctrl-C / Ctrl-Z | `ISIG` | bỏ dòng đang gõ, in `^C`/`^Z`, gửi SIGINT/SIGTSTP cho **cả nhóm tiền cảnh** |
| Ctrl-T | luôn luôn | đổi sang terminal ảo kế tiếp |
| Ctrl-P | luôn luôn | `procdump`: in pid, **pgid**, trạng thái (có `stop`) |

### Tiến trình nền đọc terminal → SIGTTIN

Trong `consoleread`, nếu `p->pgid != fgpgid` thì tiến trình gửi SIGTTIN **cho cả nhóm của nó** rồi tự dừng.
Shell sẽ báo `Stopped (tty input)`. Sau `fg`, nó trở thành tiền cảnh, đọc lại và lần này thành công.
Nếu tiến trình **bỏ qua** SIGTTIN (ví dụ shell) thì `read()` trả `-1` thay vì treo mãi.
Ghi ra terminal từ nền thì vẫn được (giống Linux mặc định khi tắt `TOSTOP`).

### Terminal ảo thứ hai (stretch goal)

Máy `virt` của QEMU chỉ có 1 UART. Vì vậy ta làm **2 terminal ảo dùng chung 1 UART**, giống Alt-F1/F2 trên Linux:

- Mỗi terminal có buffer vào, chế độ và `fgpgid` riêng. `init` chạy 1 shell trên `console` (minor 0) và 1 shell trên `tty1` (minor 1, cùng major `CONSOLE`).
- Phím gõ vào luôn đi tới terminal đang hiện (`cons.active`).
- Output của terminal đang ẩn được giữ trong `obuf` (2 KB, đầy thì mất phần cũ nhất). Khi chuyển tới, kernel in `[ttyN]`, rồi in lại phần đã giữ (hoặc dòng prompt hiện tại).

## 5. Shell (`user/sh.c`)

Tham khảo GNU libc manual, mục *Implementing a Job Control Shell*.

**Khởi động** (chỉ khi `isatty(0)`): `setpgid(0,0)` để vào nhóm riêng, `tcsetpgrp(0, mình)`,
bỏ qua SIGINT/SIGTSTP/SIGTTIN/SIGTTOU, đặt terminal về cooked.

**Mỗi dòng lệnh = 1 job:**

```
pid = fork()
con:  setpgid(0,0); nếu tiền cảnh: tcsetpgrp(0, getpid())
      signal(SIGINT/SIGTSTP/SIGTTIN/SIGTTOU, SIG_DFL); runcmd(...)
cha:  setpgid(pid,pid)   ← cả cha lẫn con đều gọi, để không phụ thuộc ai chạy trước
      thêm vào bảng job
      tiền cảnh: fgjob()   |   nền (&): in "[1] pid"
```

Tiến trình mà `runcmd` fork thêm (hai vế của `|`, các lệnh sau `;`) thừa hưởng nhóm,
nên **cả pipeline là một job** và Ctrl-C/Ctrl-Z tới tất cả cùng lúc.
Shell chỉ chờ tiến trình đầu nhóm (`pid == pgid`). Tiến trình đầu nhóm của một pipeline
đang ngủ trong `wait()`, nên `kwaitpid` cũng phải là điểm dừng, thì nó mới dừng theo.

**`fgjob(j, cont)`**: khôi phục chế độ terminal của job (nếu job từng bị dừng) → `tcsetpgrp(0, j->pgid)` →
nếu `cont` thì `sigsend(-pgid, SIGCONT)` → `waitpid(pgid, &st, WUNTRACED)` →
`tcsetpgrp(0, shell)`. Nếu job dừng thì lưu lại chế độ terminal của nó và in `[1]+ Stopped`.
Cuối cùng đặt lại chế độ cooked cho shell.

**`bg`**: `sigsend(-pgid, SIGCONT)`, không chuyển terminal.
**`jobs`**, **`kill %n`** (SIGKILL cả nhóm), **`history`**, **`exit`**, **`cd`**.
Trước mỗi prompt, `waitpid(-1, WNOHANG|WUNTRACED)` thu trạng thái job nền và in `Done`/`Stopped`.

**Line editor + lịch sử (stretch goal):** khi chờ lệnh, shell chuyển terminal sang **raw**
và tự xử lý phím: ←/→, Home/End, ^A/^E, Backspace, ^U, ↑/↓ để duyệt 16 lệnh gần nhất, ^C bỏ dòng, ^D thoát.
Mỗi lần sửa, shell vẽ lại dòng: `\r$ <dòng>\x1b[K` rồi lùi con trỏ bằng `\x1b[nD`.
Trước khi chạy lệnh, shell trả terminal về cooked cho chương trình.

## 6. Kiểm thử

- `./test-jobctl.py`: script Python chạy trên host, dùng `pexpect` để khởi động QEMU và **gõ phím thật**
  (gồm cả Ctrl-C/Z/T và phím mũi tên), rồi kiểm tra output. Gồm 12 kịch bản: Ctrl-C, Ctrl-Z→bg→fg→Ctrl-C,
  chương trình chiếm CPU, đọc từ nền (SIGTTIN), pipeline là một job, No-Echo, Raw, sửa dòng ở chế độ Cooked,
  lịch sử + sửa dòng của shell, terminal thứ hai, thoát shell khi còn job đang dừng, Ctrl-C ở prompt.
- `./test-xv6.py usertests`: toàn bộ test gốc của xv6, để chắc không làm hỏng phần cũ.

## 7. Giới hạn đã biết (nói thật khi bị hỏi)

- Không có handler tín hiệu ở user (như `sigaction`, `sigreturn`), chỉ có hành động mặc định hoặc bỏ qua. Shell không cần handler vì nó dùng `waitpid(WNOHANG)` trước mỗi prompt thay cho SIGCHLD.
- Không có *session* và *controlling terminal* đầy đủ: quyền đọc chỉ được kiểm tra bằng cách so `pgid` với `fgpgid` của terminal đang đọc. `setpgid` không kiểm tra nhóm đích có tồn tại hay không.
- Tiến trình đang ngủ ở chỗ không phải điểm dừng (ví dụ chờ đĩa) chỉ dừng khi quay về user space.
- Terminal ảo: output của terminal ẩn giữ tối đa 2 KB. Khi chuyển terminal không vẽ lại cả màn hình, chỉ vẽ lại dòng hiện tại.
- Ghi từ nền được phép (không có SIGTTOU), và `tcsetpgrp` không kiểm tra người gọi có ở tiền cảnh không.
