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

MODULE_AUTHOR("Eduard Manta");
MODULE_LICENSE("GPL");
#define EXPORT_SYMTAB
#define MODNAME "Messages system"
#define DEVICE_NAME "message-dev"

// Driver definitions
static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);

// Globals
static DEFINE_MUTEX(device_state);
static int MAJOR;
static struct list_head *list = NULL;

// Driver implementation
static int dev_open(struct inode *inode, struct file *file) {
	// this device file is single instance
	if (!mutex_trylock(&device_state)) {
		return -EBUSY;
	}

	// If it's the first time initialize the list
	if (list == NULL) INIT_LIST_HEAD_RCU(list);

	printk("%s: device file successfully opened\n",MODNAME);
	return 0;
}

static int dev_release(struct inode *inode, struct file *file) {
	// release lock
	mutex_unlock(&device_state);

	printk("%s: device file closed\n",MODNAME);
	return 0;
}

struct queue_elem {
	char c;
	struct list_head list;
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
	list_add_tail_rcu(&(elem->list), list);
	
	return 1;
}

static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off) {

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
	printk("%s: somebody called a read on dev with [MAJOR,minor] number [%d,%d]\n",
		MODNAME, MAJOR(filp->f_inode->i_rdev), MINOR(filp->f_inode->i_rdev));
#else
	printk("%s: somebody called a read on dev with [MAJOR,minor] number [%d,%d]\n",
		MODNAME, MAJOR(filp->f_dentry->d_inode->i_rdev), MINOR(filp->f_dentry->d_inode->i_rdev));
#endif

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
