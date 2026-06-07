#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p build/test_outputs

echo "== scheduling =="
./bin/scheduler --input 01_scheduling/tests/processes.csv --compare --quantum 2 | tee build/test_outputs/scheduling_compare.out
grep -q "COMPARE" build/test_outputs/scheduling_compare.out
grep -q "best_avg_waiting" build/test_outputs/scheduling_compare.out
./bin/scheduler --input 01_scheduling/tests/processes.csv --algo rr --quantum 2 | tee build/test_outputs/scheduling_rr.out
grep -q "GANTT" build/test_outputs/scheduling_rr.out
grep -q "avg_turnaround" build/test_outputs/scheduling_rr.out

echo "== memory =="
./bin/memory_lab partition --algo ff --input 02_memory/tests/partition.trace | tee build/test_outputs/memory_partition_ff.out
grep -q "external_fragmentation" build/test_outputs/memory_partition_ff.out
./bin/memory_lab partition --algo bf --input 02_memory/tests/partition.trace | tee build/test_outputs/memory_partition_bf.out
grep -q "SUMMARY" build/test_outputs/memory_partition_bf.out
./bin/memory_lab partition --algo wf --input 02_memory/tests/partition.trace | tee build/test_outputs/memory_partition_wf.out
grep -q "external_fragmentation" build/test_outputs/memory_partition_wf.out
./bin/memory_lab paging --algo fifo --input 02_memory/tests/pages.refs | tee build/test_outputs/memory_paging_fifo.out
grep -q "fault_rate" build/test_outputs/memory_paging_fifo.out
./bin/memory_lab paging --algo lru --input 02_memory/tests/pages.refs | tee build/test_outputs/memory_paging_lru.out
grep -q "faults=" build/test_outputs/memory_paging_lru.out
./bin/memory_lab paging --algo opt --input 02_memory/tests/pages.refs | tee build/test_outputs/memory_paging_opt.out
grep -q "fault_rate" build/test_outputs/memory_paging_opt.out
./bin/memory_lab paging --algo clock --input 02_memory/tests/pages.refs | tee build/test_outputs/memory_paging_clock.out
grep -q "fault_rate" build/test_outputs/memory_paging_clock.out

echo "== synchronization =="
./bin/sync_lab producer_consumer --producers 2 --consumers 2 --items 12 --buffer 4 | tee build/test_outputs/sync_pc.out
grep -q "result=PASS" build/test_outputs/sync_pc.out
./bin/sync_lab readers_writers --readers 3 --writers 2 --loops 3 | tee build/test_outputs/sync_rw.out
grep -q "result=PASS" build/test_outputs/sync_rw.out
./bin/sync_lab dining_philosophers --philosophers 5 --meals 3 | tee build/test_outputs/sync_dining.out
grep -q "result=PASS" build/test_outputs/sync_dining.out

echo "== mini filesystem =="
IMG=build/test_outputs/minifs.img
HOST_OUT=build/test_outputs/minifs_alpha.txt
rm -f "$IMG" "$HOST_OUT"
./bin/minifs format "$IMG" --blocks 96 --block-size 256 | tee build/test_outputs/minifs_format.out
./bin/minifs create "$IMG" /alpha.txt | tee build/test_outputs/minifs_create.out
./bin/minifs write "$IMG" /alpha.txt "hello" | tee build/test_outputs/minifs_write.out
./bin/minifs append "$IMG" /alpha.txt " world" | tee build/test_outputs/minifs_append.out
./bin/minifs read "$IMG" /alpha.txt | tee build/test_outputs/minifs_read.out
grep -q "hello world" build/test_outputs/minifs_read.out
./bin/minifs create "$IMG" /beta.txt | tee -a build/test_outputs/minifs_create.out
./bin/minifs write "$IMG" /beta.txt "temporary" | tee build/test_outputs/minifs_beta_write.out
./bin/minifs delete "$IMG" /beta.txt | tee build/test_outputs/minifs_delete.out
./bin/minifs ls "$IMG" | tee build/test_outputs/minifs_ls.out
grep -q "alpha.txt" build/test_outputs/minifs_ls.out
if grep -q "beta.txt" build/test_outputs/minifs_ls.out; then
    echo "beta.txt should have been deleted" >&2
    exit 1
fi
./bin/minifs readto "$IMG" /alpha.txt "$HOST_OUT" | tee build/test_outputs/minifs_readto.out
grep -q "hello world" "$HOST_OUT"
./bin/minifs stat "$IMG" | tee build/test_outputs/minifs_stat.out
grep -q "data_free" build/test_outputs/minifs_stat.out

echo "== extension scheduling/performance =="
./bin/os_perf optimize --input 06_sched_perf/tests/perf_processes.csv --quantum 3 | tee build/test_outputs/perf_optimize.out
grep -q "PASS scheduling_optimization_metrics_complete" build/test_outputs/perf_optimize.out
./bin/os_perf rt --policy rr --duration-ms 30 | tee build/test_outputs/perf_rt.out
grep -q "PASS realtime_policy_probe_complete" build/test_outputs/perf_rt.out
./bin/os_perf perf --threads 2 --iters 100000 | tee build/test_outputs/perf_system.out
grep -q "PASS system_performance_probe_complete" build/test_outputs/perf_system.out
./bin/os_perf concurrency --threads 4 --iters 50000 | tee build/test_outputs/perf_concurrency.out
grep -q "SUMMARY result=PASS" build/test_outputs/perf_concurrency.out

echo "== kernel modules compile-only verification =="
test -f 05_linux_kernel_system/hello_module/os_hello.ko
test -f 05_linux_kernel_system/chardev_proc_sysfs/os_lab_driver.ko
modinfo 05_linux_kernel_system/hello_module/os_hello.ko | tee build/test_outputs/kernel_hello_modinfo.out
modinfo 05_linux_kernel_system/chardev_proc_sysfs/os_lab_driver.ko | tee build/test_outputs/kernel_driver_modinfo.out
grep -q "description:" build/test_outputs/kernel_hello_modinfo.out
grep -q "description:" build/test_outputs/kernel_driver_modinfo.out

echo "== syscall extension compile-only verification =="
set +e
./bin/test_os_course_syscall | tee build/test_outputs/syscall_probe.out
SYSCALL_RC=${PIPESTATUS[0]}
set -e
if [[ "$SYSCALL_RC" -ne 0 && "$SYSCALL_RC" -ne 2 ]]; then
    echo "unexpected syscall test return code: $SYSCALL_RC" >&2
    exit 1
fi
grep -Eq "OK|not installed" build/test_outputs/syscall_probe.out

echo "ALL_TESTS_PASS"
