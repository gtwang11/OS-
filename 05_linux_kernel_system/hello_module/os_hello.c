#include <linux/init.h>
#include <linux/module.h>
#include <linux/utsname.h>

static char *student = "OS course";
module_param(student, charp, 0444);
MODULE_PARM_DESC(student, "Name printed when the module is loaded");

static int __init os_hello_init(void) {
    pr_info("os_hello: hello %s, kernel=%s %s\n",
            student, init_uts_ns.name.release, init_uts_ns.name.machine);
    return 0;
}

static void __exit os_hello_exit(void) {
    pr_info("os_hello: goodbye %s\n", student);
}

module_init(os_hello_init);
module_exit(os_hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS course design");
MODULE_DESCRIPTION("Minimal Linux kernel module for OS course design");
