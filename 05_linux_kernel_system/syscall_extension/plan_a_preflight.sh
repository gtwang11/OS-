#!/usr/bin/env bash
set -euo pipefail

MIN_FREE_GB="${MIN_FREE_GB:-25}"
SOURCE_DIR="${1:-${KERNEL_SOURCE_DIR:-}}"
SYSCALL_NR="${SYSCALL_NR:-462}"
PLACEHOLDER_SOURCE_DIR="/path/to/linux-source"

failures=0

check_pass() {
    printf 'PASS: %s\n' "$1"
}

check_fail() {
    printf 'FAIL: %s\n' "$1"
    failures=$((failures + 1))
}

check_warn() {
    printf 'WARN: %s\n' "$1"
}

have_cmd() {
    command -v "$1" >/dev/null 2>&1
}

printf 'PLAN_A_PREFLIGHT started\n'
printf 'running_kernel=%s\n' "$(uname -r)"
printf 'target_syscall_nr=%s\n' "$SYSCALL_NR"

if [[ "$SYSCALL_NR" =~ ^[0-9]+$ ]]; then
    check_pass "syscall number is numeric: $SYSCALL_NR"
else
    check_fail "syscall number must be numeric: $SYSCALL_NR"
fi

free_kb=$(df -Pk /home | awk 'NR==2 {print $4}')
free_gb=$((free_kb / 1024 / 1024))
if [ "$free_gb" -ge "$MIN_FREE_GB" ]; then
    check_pass "free space on /home is ${free_gb}G >= ${MIN_FREE_GB}G"
else
    check_fail "free space on /home is ${free_gb}G; full kernel build needs at least ${MIN_FREE_GB}G"
fi

if [ "$(id -u)" -eq 0 ]; then
    check_pass "running as root; install/update-grub/reboot steps are available"
elif sudo -n true >/dev/null 2>&1; then
    check_pass "sudo is available without prompting or current sudo timestamp is still valid"
elif [ -t 0 ]; then
    check_warn "sudo needs a password; prompting now to verify root access"
    if sudo -v; then
        check_pass "interactive sudo/root command execution is available"
    else
        check_fail "sudo password verification failed; install/update-grub/reboot steps need root"
    fi
else
    check_fail "sudo is not available non-interactively; run from a terminal or configure passwordless sudo for install/update-grub/reboot steps"
fi

for cmd in make gcc gcc-12 flex bison bc openssl perl fakeroot dh dpkg-checkbuilddeps dpkg-buildpackage update-grub; do
    if have_cmd "$cmd"; then
        check_pass "command available: $cmd"
    else
        check_fail "command missing: $cmd"
    fi
done

for path in /boot/config-"$(uname -r)" /boot/vmlinuz-"$(uname -r)" /boot/initrd.img-"$(uname -r)"; do
    if [ -e "$path" ]; then
        check_pass "boot artifact exists: $path"
    else
        check_fail "boot artifact missing: $path"
    fi
done

if [ "$SOURCE_DIR" = "$PLACEHOLDER_SOURCE_DIR" ]; then
    check_fail "$PLACEHOLDER_SOURCE_DIR is only a placeholder; replace it with the real extracted Linux source directory, for example /home/wgt/kernel-src/linux-hwe-6.8-6.8.0"
elif [ -z "$SOURCE_DIR" ]; then
    check_fail "no full kernel source directory supplied; set KERNEL_SOURCE_DIR or pass it as argv[1]"
else
    if [ -d "$SOURCE_DIR" ]; then
        check_pass "source directory exists: $SOURCE_DIR"
    else
        check_fail "source directory does not exist: $SOURCE_DIR"
    fi
    for rel in \
        arch/x86/entry/syscalls/syscall_64.tbl \
        include/linux/syscalls.h \
        kernel/Makefile \
        Makefile; do
        if [ -f "$SOURCE_DIR/$rel" ]; then
            check_pass "kernel source file exists: $rel"
        else
            check_fail "kernel source file missing: $rel"
        fi
    done
    tbl="$SOURCE_DIR/arch/x86/entry/syscalls/syscall_64.tbl"
    if [ -f "$tbl" ] && [[ "$SYSCALL_NR" =~ ^[0-9]+$ ]]; then
        if grep -Eq "^[[:space:]]*${SYSCALL_NR}[[:space:]]+" "$tbl" &&
           ! grep -Eq "^[[:space:]]*${SYSCALL_NR}[[:space:]]+[^[:space:]]+[[:space:]]+os_course_info[[:space:]]+" "$tbl"; then
            check_fail "syscall number $SYSCALL_NR is already used in syscall_64.tbl"
        else
            check_pass "syscall number $SYSCALL_NR is free or already assigned to os_course_info"
        fi
    fi
fi

if apt-cache search linux-source | grep -q '^linux-source-6\.8\.0'; then
    check_pass "apt index contains linux-source-6.8.0"
else
    check_warn "apt index does not contain linux-source-6.8.0; you may need deb-src, Ubuntu kernel git, or kernel.org source"
fi

printf 'PLAN_A_PREFLIGHT failures=%d\n' "$failures"
if [ "$failures" -eq 0 ]; then
    printf 'PLAN_A_PREFLIGHT result=PASS\n'
else
    printf 'PLAN_A_PREFLIGHT result=FAIL\n'
    exit 1
fi
