#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OS_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SOURCE_DIR="${1:-${KERNEL_SOURCE_DIR:-}}"
LOCALVERSION="${LOCALVERSION:--oscourse}"
JOBS="${JOBS:-$(nproc)}"
SYSCALL_NR="${SYSCALL_NR:-462}"
PLACEHOLDER_SOURCE_DIR="/path/to/linux-source"
BUILD_LOG="${BUILD_LOG:-$OS_ROOT/build/kernel_build_$(date +%Y%m%d_%H%M%S).log}"

if [ -z "$SOURCE_DIR" ]; then
    echo "Usage: KERNEL_SOURCE_DIR=/path/to/full/linux-source $0" >&2
    echo "   or: $0 /path/to/full/linux-source" >&2
    exit 2
fi
if [ "$SOURCE_DIR" = "$PLACEHOLDER_SOURCE_DIR" ]; then
    echo "$PLACEHOLDER_SOURCE_DIR is only a placeholder." >&2
    echo "Replace it with the real extracted Linux source directory, for example:" >&2
    echo "  $0 /home/wgt/kernel-src/linux-hwe-6.8-6.8.0" >&2
    exit 2
fi

"$SCRIPT_DIR/plan_a_preflight.sh" "$SOURCE_DIR"

cd "$SOURCE_DIR"

if grep -Eq "^[[:space:]]*${SYSCALL_NR}[[:space:]]+" arch/x86/entry/syscalls/syscall_64.tbl &&
   ! grep -Eq "^[[:space:]]*${SYSCALL_NR}[[:space:]]+[^[:space:]]+[[:space:]]+os_course_info[[:space:]]+" arch/x86/entry/syscalls/syscall_64.tbl; then
    echo "syscall number ${SYSCALL_NR} is already used by another entry" >&2
    exit 1
fi

if grep -q 'os_course_info' arch/x86/entry/syscalls/syscall_64.tbl; then
    echo "syscall table already contains os_course_info"
else
    tmp_tbl=$(mktemp)
    awk -v nr="$SYSCALL_NR" '
        BEGIN { added = 0 }
        !added && $1 ~ /^[0-9]+$/ && ($1 + 0) > nr {
            printf "%s\tcommon\tos_course_info\t\tsys_os_course_info\n", nr
            added = 1
        }
        { print }
        END {
            if (!added) {
                printf "%s\tcommon\tos_course_info\t\tsys_os_course_info\n", nr
            }
        }
    ' arch/x86/entry/syscalls/syscall_64.tbl > "$tmp_tbl"
    mv "$tmp_tbl" arch/x86/entry/syscalls/syscall_64.tbl
fi

if grep -q 'sys_os_course_info' include/linux/syscalls.h; then
    echo "include/linux/syscalls.h already declares sys_os_course_info"
else
    tmp_syscalls=$(mktemp)
    awk '
        { lines[NR] = $0 }
        END {
            last_endif = 0
            for (i = NR; i >= 1; --i) {
                if (lines[i] == "#endif") {
                    last_endif = i
                    break
                }
            }
            for (i = 1; i <= NR; ++i) {
                if (i == last_endif) {
                    print ""
                    print "asmlinkage long sys_os_course_info(int __user *user_pid,"
                    print "\t\t\t\t   char __user *user_comm,"
                    print "\t\t\t\t   size_t user_len);"
                }
                print lines[i]
            }
        }
    ' include/linux/syscalls.h > "$tmp_syscalls"
    mv "$tmp_syscalls" include/linux/syscalls.h
fi

cp "$SCRIPT_DIR/os_course_syscall.c" kernel/os_course_syscall.c

if grep -q 'os_course_syscall.o' kernel/Makefile; then
    echo "kernel/Makefile already builds os_course_syscall.o"
else
    printf '\nobj-y += os_course_syscall.o\n' >> kernel/Makefile
fi

if [ ! -f .config ]; then
    cp /boot/config-"$(uname -r)" .config
fi

scripts/config --set-str LOCALVERSION "$LOCALVERSION" || true
scripts/config --set-str SYSTEM_TRUSTED_KEYS "" || true
scripts/config --set-str SYSTEM_REVOCATION_KEYS "" || true
make olddefconfig

echo "Building kernel with LOCALVERSION=$LOCALVERSION syscall_nr=$SYSCALL_NR using $JOBS jobs"
mkdir -p "$(dirname "$BUILD_LOG")"
echo "Kernel build log: $BUILD_LOG"
if ! make -j"$JOBS" bindeb-pkg LOCALVERSION="$LOCALVERSION" 2>&1 | tee "$BUILD_LOG"; then
    echo "PLAN_A_BUILD result=FAILED"
    echo "Full build log: $BUILD_LOG"
    echo "Likely first error lines:"
    grep -nEi '(^|[^A-Za-z])(error:|fatal error:|No rule to make target|undefined reference|recipe for target|BTF|pahole|objtool|certs/|scripts/)' "$BUILD_LOG" | head -n 40 || true
    exit 1
fi

cd ..
echo "Installing generated linux-image/linux-headers packages"
sudo -v
sudo dpkg -i ./linux-image-*"$LOCALVERSION"*.deb ./linux-headers-*"$LOCALVERSION"*.deb
sudo update-grub

echo "PLAN_A_BUILD result=INSTALLED"
echo "Reboot into the kernel whose version contains '$LOCALVERSION', then run:"
echo "  cd /home/wgt/OS && make bin/test_os_course_syscall && ./bin/test_os_course_syscall"
