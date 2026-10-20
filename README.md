# xv6-riscv: TTY Layer & Job Control (Đề tài #27)

> **Môn học:** Hệ điều hành (Operating Systems) — Đại học Bách khoa Hà Nội (HUST)  
> **Sinh viên:** Mai Duy Hiếu  
> **MSSV:** 20223967  
> **Mã nguồn gốc:** `mit-pdos/xv6-riscv` (commit `06aad25`)

---

## 📌 Tổng quan đề tài

Dự án hiện thực hóa lớp điều khiển thiết bị đầu cuối (**TTY Layer**) và cơ chế quản lý công việc (**Job Control**) trên hệ điều hành giáo trình **xv6-riscv**, mô phỏng các tính năng cốt lõi của các hệ điều hành họ Unix/Linux chuẩn POSIX.

### Các tính năng chính đã hoàn thành:

1. **Lớp thiết bị đầu cuối (TTY Layer):**
   - Hỗ trợ 3 chế độ nhập xuất qua `ioctl` (`TCGETS`, `TCSETS`):
     - **Cooked (Canonical):** Kernel đệm dòng, hỗ trợ sửa dòng (Backspace, `^U`, `^W`), chỉ đánh thức khi gặp ký tự xuống dòng (`\n`).
     - **No-Echo:** Tương tự Cooked nhưng ẩn ký tự gõ vào (phục vụ nhập mật khẩu).
     - **Raw:** Trả về ngay lập tức từng byte gõ phím mà không qua xử lý đệm, không echo.
   - Nhóm tiến trình tiền cảnh (**Foreground Process Group**) qua `TIOCGPGRP` / `TIOCSPGRP`.
   - Ngăn chặn đọc ngầm: Tiến trình nền gọi `read()` từ TTY sẽ nhận tín hiệu `SIGTTIN` và tự động dừng.

2. **Cơ chế Tín hiệu & Quản lý Tiến trình (Signals & Process Groups):**
   - Bổ sung trường `pgid` (Process Group ID), hỗ trợ `setpgid()`, `getpgid()`.
   - Bổ sung trạng thái tiến trình `STOPPED` trong `enum procstate`.
   - Cơ chế phát và chuyển giao tín hiệu thu nhỏ: `SIGINT` (Ctrl-C), `SIGTSTP` (Ctrl-Z), `SIGCONT` (tiếp tục), `SIGHUP` (ngắt kết nối), `SIGTTIN` (tiến trình nền đọc terminal).
   - Mở rộng lệnh hệ thống `waitpid()` hỗ trợ cờ `WNOHANG`, `WUNTRACED` và trả về trạng thái chi tiết theo chuẩn POSIX.
   - Xử lý tiến trình mồ côi bị dừng (Orphaned Stopped Process) kích hoạt `SIGHUP` + `SIGCONT` khi reparent về `init`.

3. **Shell tương tác (Job Control Shell):**
   - Quản lý bảng job: Thực thi đa tiến trình tiền cảnh và hậu cảnh (hỗ trợ toán tử `&`, pipeline `|`).
   - Các lệnh nội dựng (built-in commands): `jobs`, `fg`, `bg`, `kill %n`, `history`, `exit`.
   - Tự động lưu và khôi phục chế độ TTY của từng job khi chuyển đổi giữa tiền cảnh và nền.

4. **Tính năng mở rộng (Stretch Goals):**
   - **Line Editor & History:** Soạn thảo dòng lệnh nâng cao trong chế độ Raw (di chuyển con trỏ qua phím mũi tên Trái/Phải, Home, End, `^A`, `^E`, xóa từ `^W`, xóa dòng `^U`, xem lại lịch sử lệnh qua mũi tên Lên/Xuống).
   - **Virtual Terminals (Multi-TTY):** Khởi tạo nhiều shell độc lập trên 2 terminal ảo (`console` và `tty1`), chuyển đổi linh hoạt qua phím nóng `Ctrl-T`.

---

## 📂 Cấu trúc tài liệu dự án

* [`DESIGN.md`](DESIGN.md): Tài liệu thiết kế kiến trúc kỹ thuật chi tiết (cấu trúc `proc`, các syscall mới, máy trạng thái, cơ chế ngắt và TTY buffer).
* [`DEMO.md`](DEMO.md): Kịch bản các bước chạy demo thực nghiệm chi tiết (~5 phút).
* [`test-jobctl.py`](test-jobctl.py): Kịch bản Python tự động mô phỏng phím gõ từ máy host qua thư viện `pexpect` để kiểm thử toàn diện 12 ca kiểm thử.

---

## 🚀 Hướng dẫn cài đặt & Chạy thử nghiệm

### Yêu cầu môi trường
* Hệ điều hành: Ubuntu 22.04 / 24.04 / 26.04 LTS (x86_64)
* Toolchain RISC-V: `gcc-riscv64-linux-gnu`
* Bộ giả lập phần cứng: `qemu-system-riscv64`
* Python 3 & thư viện: `python3-pexpect` (cho test tự động)

```bash
# Cài đặt gói phụ thuộc trên Ubuntu/Debian
sudo apt update
sudo apt install -y build-essential gcc-riscv64-linux-gnu qemu-system-misc python3 python3-pexpect
```

### Biên dịch & Khởi động xv6
```bash
# Xóa build cũ và biên dịch lại
make clean
make qemu
```

> **Cách thoát QEMU:** Nhấn tổ hợp phím `Ctrl-A` rồi bấm phím `x`.

---

## 🧪 Kiểm thử (Testing)

### 1. Kiểm thử tự động tính năng TTY & Job Control
Kịch bản test tự động khởi động QEMU và tự động giả lập bấm phím (Ctrl-C, Ctrl-Z, bg, fg, Ctrl-T, mũi tên, pipe, signals):
```bash
python3 test-jobctl.py
```
*(Kết quả mong đợi: `All 12 tests passed!`)*

### 2. Kiểm thử tương thích toàn bộ hệ thống gốc xv6
Đảm bảo các thay đổi không phá vỡ bất kỳ tính năng tiêu chuẩn nào của xv6:
```bash
$ usertests
```
*(Kết quả mong đợi: `ALL TESTS PASSED`)*
