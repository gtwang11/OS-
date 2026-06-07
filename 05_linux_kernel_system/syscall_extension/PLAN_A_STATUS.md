# Plan A Status: Build And Boot Patched Kernel

## Current Result

Plan A has been completed.

The `os_course_info` syscall implementation, Linux 6.8 patch, preflight/build scripts, kernel build, package installation, reboot, and user-space runtime verification have all been completed.

## Verified Runtime State

- Booted kernel: `6.8.12-oscourse-oscourse`.
- Syscall number: `462`.
- User-space test in a normal terminal:

```text
SYSCALL os_course_info -> OK pid=4219 comm=test_os_course_ nr=462
```

- User-space test in a PID namespace isolated environment may show a different namespace PID, for example:

```text
SYSCALL os_course_info -> OK pid=10384 user_pid=2 comm=test_os_course_ nr=462 note=pid_namespace
```

This still verifies that the patched kernel installed syscall number `462` and returned a valid kernel PID and process name.

## Notes From The Build

- The full kernel build took a long time because it used an Ubuntu generic kernel configuration with many modules and debug/BTF options enabled.
- The script clears `CONFIG_SYSTEM_TRUSTED_KEYS` and `CONFIG_SYSTEM_REVOCATION_KEYS` before `olddefconfig`, avoiding the local-build failure caused by missing Ubuntu Canonical certificate files such as `debian/canonical-certs.pem`.
- The build log is written under `OS/build/kernel_build_*.log`.

## Revalidation Commands

```bash
uname -r
cd /home/wgt/OS
make bin/test_os_course_syscall
./bin/test_os_course_syscall
```

Passing criteria:

- `uname -r` contains `-oscourse`.
- The syscall test prints `SYSCALL os_course_info -> OK`.
