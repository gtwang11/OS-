#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

SYSCALL_DEFINE3(os_course_info, int __user *, user_pid,
		char __user *, user_comm, size_t, user_len)
{
	int pid = task_pid_nr(current);
	char comm[TASK_COMM_LEN];
	size_t copy_len;

	if (!user_pid || !user_comm || user_len == 0)
		return -EINVAL;

	get_task_comm(comm, current);
	copy_len = strnlen(comm, TASK_COMM_LEN - 1);
	if (copy_len + 1 > user_len)
		return -ENAMETOOLONG;

	if (copy_to_user(user_pid, &pid, sizeof(pid)))
		return -EFAULT;
	if (copy_to_user(user_comm, comm, copy_len + 1))
		return -EFAULT;
	return 0;
}
