# Kịch bản demo (khoảng 5 phút)

Chuẩn bị: `make clean && make qemu`. Thoát QEMU bằng `Ctrl-A` rồi `x`.

## 1. Yêu cầu chính của đề: Ctrl-Z → bg → fg → Ctrl-C

```
$ counter                 ← in số mỗi giây
counter[4]: 1
counter[4]: 2
^Z                        ← Ctrl-Z
[1]+ Stopped	counter
$ jobs
[1]+ Stopped	counter
  (nhấn Ctrl-P: dòng "4 4 stop   counter" — kernel thấy trạng thái STOPPED, pgid 4)
$ bg
[1]+ Running	counter &
counter[4]: 3             ← vẫn chạy, nhưng shell đã nhận lệnh khác
$ fg
counter
counter[4]: 5
^C                        ← Ctrl-C
$
```

## 2. Pipeline là một job

```
$ counter | cat
^Z
[1]+ Stopped	counter | cat
  (Ctrl-P: sh, counter, cat cùng một pgid, đều "stop")
$ fg
^C
```

## 3. Chương trình chiếm CPU (dừng nhờ ngắt timer)

```
$ spin
.....^Z
[1]+ Stopped	spin
$ kill %1
$ jobs
[1]+ Killed	spin
```

## 4. Tiến trình nền đọc bàn phím → SIGTTIN

```
$ cat &
[1] 7
$ jobs
[1]+ Stopped (tty input)	cat
$ fg
cat
hello                     ← gõ
hello                     ← cat in lại
(Ctrl-D)
```

## 5. Ba chế độ terminal qua ioctl

```
$ ttydemo noecho          ← gõ mật khẩu, không hiện chữ
$ ttydemo raw             ← mỗi phím hiện mã ngay; Ctrl-C chỉ là "key ^C = 3"; q để thoát
$ ttydemo cooked          ← thử Backspace, Ctrl-U, Ctrl-W
```

## 6. Stretch goal

- **Lịch sử lệnh:** ↑/↓ để duyệt, ←/→ để sửa giữa dòng, gõ `history` để xem danh sách.
- **Terminal thứ hai:** Ctrl-T → `[tty1]`, chạy `counter` ở đó, Ctrl-T quay về `[tty0]` (output của tty1 không lẫn vào), Ctrl-T lần nữa sẽ thấy phần output đã được giữ lại.

## 7. Test tự động

```
./test-jobctl.py          # 12 kịch bản gõ phím, khoảng 2 phút
./test-xv6.py usertests   # test gốc của xv6, khoảng 5 phút
```
