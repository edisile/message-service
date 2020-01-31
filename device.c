#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>	
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/tty.h>
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
NEW_LF_QUEUE(queue);

// Driver implementation
static int dev_open(struct inode *inode, struct file *file) {
	printk("%s: device file successfully opened\n",MODNAME);
	return 0;
}

static int dev_release(struct inode *inode, struct file *file) {
	printk("%s: device file closed\n",MODNAME);
	return 0;
}

#define VALID 0
#define INVALID 1

struct queue_elem {
	char c;
	struct lf_queue_node list;
	atomic_t state;
};

static ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *off) {
	struct queue_elem *elem;

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
		printk("%s: somebody called a write on dev with [MAJOR,minor] number [%d,%d]\n",
			MODNAME, MAJOR(filp->f_inode->i_rdev), MINOR(filp->f_inode->i_rdev));
		#else
		printk("%s: somebody called a write on dev with [MAJOR,minor] number [%d,%d]\n",
			MODNAME, MAJOR(filp->f_dentry->d_inode->i_rdev), MINOR(filp->f_dentry->d_inode->i_rdev));
	#endif

	// Allocation can block
	elem = (struct queue_elem *) kmalloc(sizeof(struct queue_elem), GFP_KERNEL);
	elem->c = buff[0];
	atomic_set(&(elem->state), VALID);
	lf_queue_push(&queue, (&(elem->list)));
	
	return 1;
}

static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off) {
	struct lf_queue_node *elem = lf_queue_pull(&queue);

	#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
		printk("%s: somebody called a read on dev with [MAJOR,minor] number [%d,%d]\n",
			MODNAME, MAJOR(filp->f_inode->i_rdev), MINOR(filp->f_inode->i_rdev));
		#else
		printk("%s: somebody called a read on dev with [MAJOR,minor] number [%d,%d]\n",
			MODNAME, MAJOR(filp->f_dentry->d_inode->i_rdev), MINOR(filp->f_dentry->d_inode->i_rdev));
	#endif

	if (elem != NULL) {
		struct queue_elem *x = container_of(elem, struct queue_elem, list);
		printk("Read: '%c'", x->c);
		kfree(x);
	}

	return 0;
}

// Driver in a struct
static struct file_operations fops = {
	.write = dev_write,
	.read = dev_read,
	.open =  dev_open,
	.release = dev_release
};

int init_module(void) {

	MAJOR = __register_chrdev(0, 0, 256, DEVICE_NAME, &fops);

	if (MAJOR < 0) {
		printk("%s: registering device failed\n",MODNAME);
		return MAJOR;
	}

	printk(KERN_INFO "%s: new device registered, it is assigned MAJOR number %d\n",MODNAME, MAJOR);

	return 0;
}

void cleanup_module(void) {

	unregister_chrdev(MAJOR, DEVICE_NAME);

	printk(KERN_INFO "%s: new device unregistered, it was assigned MAJOR number %d\n",MODNAME, MAJOR);

	return;

}
