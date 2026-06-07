#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef OS_COURSE_SYSCALL_NR
#define OS_COURSE_SYSCALL_NR 462
#endif

int main(void) {
    int pid = -1;
    int user_pid = getpid();
    char comm[64] = {0};
    long rc = syscall(OS_COURSE_SYSCALL_NR, &pid, comm, sizeof(comm));
    if (rc == 0) {
        if (pid <= 0 || comm[0] == '\0') {
            printf("SYSCALL os_course_info -> BAD_DATA pid=%d user_pid=%d comm=%s nr=%d\n",
                   pid, user_pid, comm, OS_COURSE_SYSCALL_NR);
            return 1;
        }
        if (pid == user_pid) {
            printf("SYSCALL os_course_info -> OK pid=%d comm=%s nr=%d\n", pid, comm, OS_COURSE_SYSCALL_NR);
        } else {
            printf("SYSCALL os_course_info -> OK pid=%d user_pid=%d comm=%s nr=%d note=pid_namespace\n",
                   pid, user_pid, comm, OS_COURSE_SYSCALL_NR);
        }
        return 0;
    }
    printf("SYSCALL os_course_info -> not installed or failed nr=%d errno=%d (%s)\n",
           OS_COURSE_SYSCALL_NR, errno, strerror(errno));
    return errno == ENOSYS ? 2 : 1;
}
