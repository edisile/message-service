#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "libs/lf_queue/lf_queue.h"

MODULE_AUTHOR("Eduard Manta");
MODULE_LICENSE("GPL");
#define EXPORT_SYMTAB
#define MODNAME "Messages system"
#define DEVICE_NAME "message-dev"

// Driver definitions
static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off);

// Globals
static int MAJOR;
DEFINE_LF_QUEUE(queue);
static long max_message_size = 512;
static long max_storage_size = 4096;
static atomic_t stored_bytes = {.counter = 0};

// Module parameters exposed via the /sys/ pseudo-fs
module_param(max_message_size, long, 0660);
module_param(max_storage_size, long, 0660);

// Driver implementation
static int dev_open(struct inode *inode, struct file *filp) {
	printk("%s: device file opened\n",MODNAME);
	return 0;
}

static int dev_release(struct inode *inode, struct file *filp) {
	printk("%s: device file closed\n",MODNAME);
	return 0;
}

struct queue_elem {
	char *message;
	unsigned long message_len;
	struct lf_queue_node list;
};

static ssize_t dev_write(struct file *filp, const char *buff, size_t len, 
			loff_t *off) {
	struct queue_elem *elem;
	unsigned long failed;
	ssize_t retval;

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
		printk("%s: write on [%d,%d]\n", MODNAME, 
			MAJOR(filp->f_inode->i_rdev), 
			MINOR(filp->f_inode->i_rdev));
		#else
		printk("%s: write on [%d,%d]\n", MODNAME, 
			MAJOR(filp->f_dentry->d_inode->i_rdev), 
			MINOR(filp->f_dentry->d_inode->i_rdev));
	#endif

	if (len > min(max_message_size, max_storage_size - stored_bytes.counter)) {
		retval = -ENOSPC;
		goto exit;
	}

	// Allocation could block because GFP_KERNEL but it's not a problem
	elem = kmalloc(sizeof(struct queue_elem), GFP_KERNEL);
	elem->message = kmalloc(len, GFP_KERNEL);

	printk("	%lu bytes to write", len);
	failed = copy_from_user(elem->message, buff, len);
	retval = elem->message_len = len - failed;
	lf_queue_push(&queue, (&(elem->list)));
	atomic_add(retval, &stored_bytes); // Keep track of used space in device
	
	exit:
	return retval;
}

static ssize_t dev_read(struct file *filp, char *buff, size_t len, 
			loff_t *off) {
	struct lf_queue_node *node;
	struct queue_elem *elem;
	ssize_t retval = 0;

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
		printk("%s: read on [%d,%d]\n",
			MODNAME, MAJOR(filp->f_inode->i_rdev), 
			MINOR(filp->f_inode->i_rdev));
		#else
		printk("%s: read on [%d,%d]\n",
			MODNAME, MAJOR(filp->f_dentry->d_inode->i_rdev), 
			MINOR(filp->f_dentry->d_inode->i_rdev));
	#endif
	
	node = lf_queue_pull(&queue);

	if (node != NULL) {
		elem = container_of(node, struct queue_elem, list);
		retval = len = min(len, elem->message_len);
		printk("	%lu bytes to read", len);

		copy_to_user(buff, elem->message + *off, len);
	
		atomic_sub(elem->message_len, &stored_bytes); // Space being freed
		kfree(elem->message);
		kfree(elem);
	}

	return retval;
}

// Driver in a struct
static struct file_operations f_ops = {
	.write = dev_write,
	.read = dev_read,
	.open =  dev_open,
	.release = dev_release,
	.unlocked_ioctl = NULL,
	.flush = NULL
};

int init_module(void) {
	MAJOR = __register_chrdev(0, 0, 256, DEVICE_NAME, &f_ops);

	if (MAJOR < 0) {
		printk(KERN_ERR "%s: register failed with major %d\n", MODNAME, MAJOR);
		return MAJOR;
	}

	printk(KERN_INFO "%s: registered as %d\n", MODNAME, MAJOR);
	return 0;
}

void cleanup_module(void) {
	unregister_chrdev(MAJOR, DEVICE_NAME);

	printk(KERN_INFO "%s: unregistered\n", MODNAME);
	return;
}
