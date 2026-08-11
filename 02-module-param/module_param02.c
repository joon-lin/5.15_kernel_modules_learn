#include <linux/init.h>
#include <linux/module.h>

static int count = 1;
static char *message = "hello from module parameter";

module_param(count, int, 0644);
MODULE_PARM_DESC(count, "An integer module parameter");

module_param(message, charp, 0644);
MODULE_PARM_DESC(message, "A string module parameter");

static int __init module_param02_init(void)
{
	pr_info("module_param02: loaded\n");
	pr_info("module_param02: count=%d, message=\"%s\"\n",
		count, message);
	return 0;
}

static void __exit module_param02_exit(void)
{
	pr_info("module_param02: count=%d, message=\"%s\"\n",
		count, message);
	pr_info("module_param02: unloaded\n");
}

module_init(module_param02_init);
module_exit(module_param02_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 02: kernel module parameters");
