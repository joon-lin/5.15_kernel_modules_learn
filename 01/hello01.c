#include <linux/init.h>
#include <linux/module.h>

static int __init hello01_init(void)
{
	pr_info("hello01: module loaded\n");
	return 0;
}

static void __exit hello01_exit(void)
{
	pr_info("hello01: module unloaded\n");
}

module_init(hello01_init);
module_exit(hello01_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Minimal kernel module example 01");
