# Linux 内核与系统编程拓展说明

本目录覆盖课程设计拓展（1）的 4 类内容，并明确区分当前普通用户环境可验证的部分和需要 root/重启内核的部分。

## 当前环境判断

- 已安装当前运行内核对应的 headers：`/lib/modules/$(uname -r)/build`。
- 当前系统只有 `gcc 11.4`，headers 记录的内核编译器是 `gcc-12`。模块 Makefile 使用 `scripts/kbuild-gcc-wrapper.sh` 过滤 gcc-12 专属的 `-ftrivial-auto-var-init=zero` 后调用本机 gcc，因此会出现编译器版本 warning；这是环境限制，不写成完全同编译器复现。
- 普通用户可完成内核模块编译；加载/卸载模块、建设备节点、安装新内核和重启验证需要 root/sudo 权限。
- 已在完整 Linux 6.8 源码树中完成系统调用扩展构建、安装和重启验证；当前运行内核为 `6.8.12-oscourse-oscourse`。
- `kernel.modules_disabled=0`，因此在拥有 root 权限时可以加载本目录编译出的模块。

## 已实现内容

- `hello_module/`：最小 Linux 内核模块，验证模块初始化、退出、模块参数和 `dmesg` 日志。
- `chardev_proc_sysfs/`：字符设备驱动，包含 `/dev/os_lab_char`、`/proc/os_lab_info` 和 `/sys/class/os_lab/os_lab_char/{stats,reset}`。
- `syscall_extension/`：Linux 6.8 新增系统调用的补丁、内核源文件、预检/构建脚本和用户态测试程序。它需要完整内核源码、配置、编译安装并重启到新内核后才能真实验证；当前普通用户环境不能安全完成这一步。

## 验证边界

- 普通用户可执行：`make kernel_modules`，判断 `.ko` 是否生成、`modinfo` 是否能读取元数据。
- root 用户可执行：进入对应模块目录运行 `sudo ./test_root.sh`，判断 `/dev`、`/proc`、`/sys` 输出是否符合脚本末尾 `PASS`。
- 系统调用扩展已在自编译 patched kernel 中验证。当前 Linux 6.8 x86_64 默认调用号为 `462`，用户态测试应输出 `SYSCALL os_course_info -> OK ... nr=462`；可通过 `SYSCALL_NR` 覆盖目标调用号。
