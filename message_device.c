#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

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
static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static int dev_flush(struct file *filp, void *id);

// This will hold the messages in a queue
struct queue_elem {
	char *message;
	unsigned long mess_len;
	struct lf_queue_node list;
};

struct session_data {
	unsigned long send_timeout, recv_timeout;
	struct lf_queue queue;
};

// ioctl commands
enum ioctl_cmds {
	SET_SEND_TIMEOUT = 99,
	SET_RECV_TIMEOUT,
	REVOKE_DELAYED_MESSAGES
};

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
	// TODO: increment module usage
	printk("%s: device file opened\n", MODNAME);
	return 0;
}

static int dev_release(struct inode *inode, struct file *filp) {
	// TODO: decrement module usage

	// private_data could contain data about the session
	if (filp->private_data != NULL) {
		vfree(filp->private_data);
		filp->private_data = NULL;
	}

	printk("%s: device file closed\n", MODNAME);
	return 0;
}

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

	// Allocate memory for the message
	if ((elem = vmalloc(sizeof(struct queue_elem))))
		elem->message = vmalloc(len);
	
	if (elem == NULL || elem->message == NULL) {
		// Allocations failed, exit with an error
		retval = -ENOMEM;
		goto exit;
	}

	printk("	%lu bytes to write", len);
	failed = copy_from_user(elem->message, buff, len);
	retval = elem->mess_len = len - failed;
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
		retval = len = min(len, elem->mess_len);
		printk("	%lu bytes to read", len);

		copy_to_user(buff, elem->message + *off, len);
	
		atomic_sub(elem->mess_len, &stored_bytes); // Space being freed
		vfree(elem->message);
		vfree(elem);
	}

	return retval;
}

static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	long retval = 0;
	struct session_data *s_data;

	printk("%s: called with cmd %u and arg %lu", MODNAME, cmd, arg);

	if (cmd == SET_SEND_TIMEOUT || cmd == SET_SEND_TIMEOUT) {
		retry:
		if (filp->private_data == NULL) {
			s_data = vmalloc(sizeof(struct session_data));
			if (s_data == NULL) {
				s_data->queue = NEW_LF_QUEUE;
				filp->private_data = s_data; // TODO: make this with an atomic CAS
				// if still NULL, change filp->private_data with s_data, else 
				// dealloc and go to retry just to be sure
			} else {
				retval = -ENOMEM;
				goto exit;
			}
		}
	}

	s_data = filp->private_data;

	switch (cmd) {
	case SET_SEND_TIMEOUT:
		s_data->send_timeout = arg;
		// TODO: find a way to make this timeout a real thing
		break;
	case SET_RECV_TIMEOUT:
		s_data->recv_timeout = arg;
		// TODO: find a way to make this timeout a real thing
		break;
	case REVOKE_DELAYED_MESSAGES:
		// TODO: implement for real
		break;
	default:
		retval = -EINVAL;
		goto exit;
	}

	exit:
	return retval;
}

static int dev_flush(struct file *filp, void *id) {
	printk("%s: flush requested", MODNAME);
	// TODO: implement for real

	return 0;
}

// Driver in a struct
static struct file_operations f_ops = {
	.write = dev_write,
	.read = dev_read,
	.open =  dev_open,
	.release = dev_release,
	.unlocked_ioctl = dev_ioctl,
	.flush = dev_flush
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
