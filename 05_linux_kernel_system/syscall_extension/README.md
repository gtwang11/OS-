# 系统调用扩展验证说明

本目录给出 Linux 6.8 x86_64 上新增 `os_course_info` 系统调用的实现、补丁和验证脚本。

实现目标：

- 新系统调用号默认为 `462`，这是当前 Linux 6.8 x86_64 native syscall 表的下一个空号；如目标源码已经占用该号，可设置 `SYSCALL_NR` 覆盖。
- 函数名为 `sys_os_course_info`。
- 用户传入 `int *pid`、`char *comm`、`size_t len`。
- 内核返回当前进程 PID 和 `comm` 名称。
- 用户态测试程序 `test_os_course_syscall.c` 通过 `syscall(462, ...)` 调用，并核对返回 PID 为正数、`comm` 非空；在普通终端中 PID 应与 `getpid()` 一致，在 PID namespace 隔离环境中可能显示 `note=pid_namespace`。

真实验证步骤必须在可控实验机或虚拟机中完成：

1. 准备完整 Linux 6.8 内核源码，而不是仅有 headers。
2. 将 `os_course_syscall.c` 放到源码树 `kernel/`。
3. 按 `linux-6.8-os-course-syscall.patch` 修改 `syscall_64.tbl`、`include/linux/syscalls.h` 和 `kernel/Makefile`，或运行 `plan_a_build_and_install.sh` 自动修改。
4. 编译、安装并重启到新内核。
5. 编译本目录的 `test_os_course_syscall.c` 并运行。

通过标准：

- 重启到 patched kernel 后，测试输出应包含 `SYSCALL os_course_info -> OK`。
- 如果未重启到 patched kernel，输出 `not installed` 或 errno 为 `ENOSYS` 是预期结果，不能写成已经完成内核运行验证。

当前已完成验证：

- `uname -r` 输出 `6.8.12-oscourse-oscourse`。
- `/home/wgt/OS/bin/test_os_course_syscall` 在真实终端输出 `SYSCALL os_course_info -> OK pid=4219 comm=test_os_course_ nr=462`。
- 在 PID namespace 隔离环境中运行同一测试时可能显示 `user_pid=... note=pid_namespace`，但仍返回 `OK`，表示 syscall 已返回有效内核 PID 和进程名。

当前环境预检：

- `plan_a_preflight.sh` 已确认 `/home` 空间满足 25G 要求。
- 当前 `sudo` 被系统策略阻止，缺 `gcc-12`、`flex`、`bison`，且没有完整内核源码树。
- 因此当前环境只能完成源码、补丁、编译测试程序和预检脚本；真实安装/重启验证必须在具备 root 权限的实验机或虚拟机中完成。

## 准备真实源码目录

不要把 `/path/to/linux-source` 原样传给脚本；它只是占位符。真实源码目录必须至少包含：

- `arch/x86/entry/syscalls/syscall_64.tbl`
- `include/linux/syscalls.h`
- `kernel/Makefile`
- 顶层 `Makefile`

当前运行内核 `6.8.0-124-generic` 的 headers 来自 Ubuntu 源码包 `linux-hwe-6.8`。在有 root 和网络的实验机上，优先用 apt source 获取匹配 Ubuntu 补丁的源码：

```bash
sudo apt update
sudo apt install -y gcc-12 flex bison libssl-dev libelf-dev dwarves bc fakeroot dpkg-dev debhelper build-essential

sudo sed -i 's/^# deb-src/deb-src/' /etc/apt/sources.list
sudo apt update

mkdir -p /home/wgt/kernel-src
cd /home/wgt/kernel-src
apt source linux-hwe-6.8
```

然后寻找真实源码目录：

```bash
find /home/wgt/kernel-src -path '*/arch/x86/entry/syscalls/syscall_64.tbl' -print
```

假设输出为：

```text
/home/wgt/kernel-src/linux-hwe-6.8-6.8.0/arch/x86/entry/syscalls/syscall_64.tbl
```

则真正传给脚本的是去掉后缀后的目录：

```bash
cd /home/wgt/OS/05_linux_kernel_system/syscall_extension
./plan_a_preflight.sh /home/wgt/kernel-src/linux-hwe-6.8-6.8.0
./plan_a_build_and_install.sh /home/wgt/kernel-src/linux-hwe-6.8-6.8.0
```

如果 apt source 仍拿不到 `linux-hwe-6.8`，可改用 Ubuntu kernel git 获取源码，但要确认源码版本与目标内核 ABI 兼容，并重新运行 `plan_a_preflight.sh`。
