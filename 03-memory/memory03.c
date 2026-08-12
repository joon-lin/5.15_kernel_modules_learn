#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

static unsigned int size = 1024;
static char *buffer;

module_param(size, uint, 0644);
MODULE_PARM_DESC(size, "Number of bytes to allocate");

static int __init memory03_init(void)
{
	/* kzalloc() allocates kernel memory and clears it to zero. */
	// buffer = kzalloc(size, GFP_KERNEL);
	buffer = vzalloc(size);
	if (!buffer) {
		pr_err("memory03: kzalloc(%u) failed\n", size);
		return -ENOMEM;
	}

	pr_info("memory03: allocated %u bytes, first byte=0x%02x\n",
		size, buffer[0]);
	return 0;
}

static void __exit memory03_exit(void)
{
	/* Every successful allocation must be released. */
	// kfree(buffer);
	vfree(buffer);
	buffer = NULL;
	pr_info("memory03: memory freed\n");
}

module_init(memory03_init);
module_exit(memory03_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SI");
MODULE_DESCRIPTION("Lesson 03: kernel memory allocation");
