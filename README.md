# 操作系统课程设计实现说明

本工程根据 `操作系统课程设计内容与要求1.pdf` 完成：

- 基础必做部分 70%：处理机调度、内存管理、进程同步与并发控制、文件系统。
- 拓展部分 30% 的（1）：Linux 内核与系统编程。
- 拓展部分 30% 的（2）：调度与性能优化。

当前目录没有伪造 GitHub URL。提交报告时请先把本目录上传到你自己的代码托管仓库，再把真实 URL 和访问方式写入报告。

## 环境结论

已验证当前环境具备：

- `gcc`、`make`、`pthread`、POSIX semaphore，可完成所有基础部分。
- 当前运行内核 headers：`/lib/modules/$(uname -r)/build`，可编译内核模块。
- 普通用户可完成基础实验、用户态拓展和内核模块编译；加载内核模块、安装新内核、更新 GRUB 和重启验证需要 root/sudo 权限。
- 已在完整 Linux 6.8 源码树中完成系统调用补丁构建、安装和重启验证；当前运行内核为 `6.8.12-oscourse-oscourse`。

因此，拓展（1）的内核模块、字符设备、`/proc`、`/sys` 已做到可编译，并提供 root 验证脚本；系统调用扩展已经完成源码、Linux 6.8 补丁、构建安装脚本和 patched kernel 运行验证。

## 目录对应关系

- `01_scheduling/`：FCFS、SJF、SRTF、RR、非抢占优先级、抢占优先级、HRRN、自适应 RR。
- `02_memory/`：动态分区 FF/BF/WF，页面置换 FIFO/LRU/OPT/Clock。
- `03_sync/`：生产者-消费者、读者-写者、公平限流的哲学家进餐。
- `04_filesystem/`：可落盘 miniFS，包含超级块、inode 表、块位图、直接块、根目录文件操作。
- `05_linux_kernel_system/`：内核模块、字符设备、`/proc`、`/sys`、系统调用扩展。
- `06_sched_perf/`：调度优化、实时调度探测、系统性能测试、并发性能优化。
- `scripts/run_all_tests.sh`：自动验收脚本。
- `docs/实验报告.md`：报告素材和验收记录模板。

## 一键编译与测试

在 `OS` 目录运行：

```bash
make test
```

通过标准：

- 终端最后出现 `ALL_TESTS_PASS`。
- `build/test_outputs/` 下生成每个模块的输出文件。
- 输出中基础模块应出现 `SUMMARY`、`fault_rate`、`result=PASS`、`hello world` 等判定字段。
- 内核模块部分会生成 `.ko` 并通过 `modinfo` 读取描述信息；这表示编译验证通过，但不表示已加载进运行内核。

只编译用户态程序：

```bash
make all
```

只编译内核模块：

```bash
make kernel_modules
```

## 手工验证命令

处理机调度：

```bash
./bin/scheduler --input 01_scheduling/tests/processes.csv --compare --quantum 2
./bin/scheduler --input 01_scheduling/tests/processes.csv --algo rr --quantum 2
```

判断方法：

- `GANTT` 展示进程运行顺序。
- `RESULTS` 中每个进程有 `start/finish/turnaround/waiting/response`。
- `SUMMARY` 中平均周转、平均等待、平均响应和切换次数用于比较算法性能。
- `ANALYSIS best_avg_waiting=...` 给出该输入下的指标最优算法。

内存管理：

```bash
./bin/memory_lab partition --algo ff --input 02_memory/tests/partition.trace
./bin/memory_lab partition --algo bf --input 02_memory/tests/partition.trace
./bin/memory_lab partition --algo wf --input 02_memory/tests/partition.trace
./bin/memory_lab paging --algo fifo --input 02_memory/tests/pages.refs
./bin/memory_lab paging --algo lru --input 02_memory/tests/pages.refs
./bin/memory_lab paging --algo opt --input 02_memory/tests/pages.refs
./bin/memory_lab paging --algo clock --input 02_memory/tests/pages.refs
```

判断方法：

- 分区实验每个 `STEP` 后都有 `MEMORY`，可看到分配、释放、合并空闲区。
- `external_fragmentation`、`largest_free`、`free_blocks` 用于判断碎片。
- 页面置换每个 `REF` 都标明 `HIT` 或 `FAULT`，`SUMMARY faults=... fault_rate=...` 是缺页统计。

进程同步：

```bash
./bin/sync_lab producer_consumer --producers 2 --consumers 2 --items 12 --buffer 4
./bin/sync_lab readers_writers --readers 3 --writers 2 --loops 3
./bin/sync_lab dining_philosophers --philosophers 5 --meals 3
```

判断方法：

- 生产者-消费者必须 `produced == consumed == items`，`duplicates=0`，`missing=0`。
- 读者-写者必须 `final_value == writers * loops`。
- 哲学家进餐必须每个哲学家吃到指定次数。
- 三者最终都应输出 `result=PASS`。

文件系统：

```bash
./bin/minifs format build/demo.img --blocks 96 --block-size 256
./bin/minifs create build/demo.img /alpha.txt
./bin/minifs write build/demo.img /alpha.txt "hello"
./bin/minifs append build/demo.img /alpha.txt " world"
./bin/minifs read build/demo.img /alpha.txt
./bin/minifs stat build/demo.img
./bin/minifs bitmap build/demo.img
```

判断方法：

- `read` 应输出 `hello world`。
- `stat` 应显示 `files`、`data_used`、`data_free`、`metadata_blocks`。
- `bitmap` 中 `1` 表示已占用块，`0` 表示空闲块；删除文件后空闲块数量会增加。

拓展（1）内核模块：

```bash
make kernel_modules
modinfo 05_linux_kernel_system/hello_module/os_hello.ko
modinfo 05_linux_kernel_system/chardev_proc_sysfs/os_lab_driver.ko
```

有 root 权限时可进一步运行：

```bash
cd 05_linux_kernel_system/hello_module
sudo ./test_root.sh

cd ../chardev_proc_sysfs
sudo ./test_root.sh
```

判断方法：

- `hello_module` 的 `dmesg` 应出现 `os_hello: hello...` 和 `os_hello: goodbye...`。
- 字符设备脚本应能读写 `/dev/os_lab_char`。
- `/proc/os_lab_info` 和 `/sys/class/os_lab/os_lab_char/stats` 应显示容量、长度、open/read/write 计数。
- `reset` 后 stats 计数和长度应归零。

拓展（1）系统调用扩展：

```bash
./bin/test_os_course_syscall
05_linux_kernel_system/syscall_extension/plan_a_preflight.sh
```

判断方法：

- 当前已重启到 patched kernel，`uname -r` 应输出 `6.8.12-oscourse-oscourse`。
- `./bin/test_os_course_syscall` 应输出 `SYSCALL os_course_info -> OK pid=... comm=... nr=462`；在 PID namespace 隔离环境中可能额外显示 `user_pid=... note=pid_namespace`，仍表示 syscall 返回有效 PID 和进程名。
- 如果未重启到 patched kernel，用户态测试会输出 `errno=38 (Function not implemented)`，那说明运行内核尚未包含新 syscall。
- 当前 Linux 6.8 x86_64 native syscall 默认使用 `462`；如目标内核源码已经占用该号，可设置 `SYSCALL_NR=其他空号` 后重新预检和构建。

拓展（2）调度与性能：

```bash
./bin/os_perf optimize --input 06_sched_perf/tests/perf_processes.csv --quantum 3
./bin/os_perf rt --policy rr --duration-ms 30
./bin/os_perf perf --threads 2 --iters 100000
./bin/os_perf concurrency --threads 4 --iters 50000
```

判断方法：

- `optimize` 中 `hrrn_optimized` 和 `adaptive_rr_optimized` 是改进算法，输出平均指标和最佳等待时间。
- `rt` 在无 root 时通常输出 `SKIP_PERMISSION_OR_POLICY`，这是诚实的权限边界；程序仍会完成实时调度参数探测。
- `perf` 输出 wall/user/sys 时间、吞吐量、上下文切换数。
- `concurrency` 比较 mutex、atomic、分片局部累加，三种总数都等于 expected 且 `SUMMARY result=PASS`。
