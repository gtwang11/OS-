CC ?= gcc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -O2 -g
PTHREAD := -pthread

.PHONY: all basics extensions kernel_modules test clean dirs

all: basics extensions

dirs:
	mkdir -p bin build

basics: dirs bin/scheduler bin/memory_lab bin/sync_lab bin/minifs

extensions: dirs bin/os_perf bin/test_os_course_syscall

bin/scheduler: 01_scheduling/src/scheduler.c | dirs
	$(CC) $(CFLAGS) -o $@ $<

bin/memory_lab: 02_memory/src/memory_lab.c | dirs
	$(CC) $(CFLAGS) -o $@ $<

bin/sync_lab: 03_sync/src/sync_lab.c | dirs
	$(CC) $(CFLAGS) $(PTHREAD) -o $@ $<

bin/minifs: 04_filesystem/src/minifs.c | dirs
	$(CC) $(CFLAGS) -o $@ $<

bin/os_perf: 06_sched_perf/src/os_perf.c | dirs
	$(CC) $(CFLAGS) $(PTHREAD) -o $@ $<

bin/test_os_course_syscall: 05_linux_kernel_system/syscall_extension/test_os_course_syscall.c | dirs
	$(CC) $(CFLAGS) -o $@ $<

kernel_modules:
	$(MAKE) -C 05_linux_kernel_system/hello_module modules
	$(MAKE) -C 05_linux_kernel_system/chardev_proc_sysfs modules

test: all kernel_modules
	bash scripts/run_all_tests.sh

clean:
	rm -rf bin build
	$(MAKE) -C 05_linux_kernel_system/hello_module clean
	$(MAKE) -C 05_linux_kernel_system/chardev_proc_sysfs clean
