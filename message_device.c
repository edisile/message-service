#include <linux/atomic.h>
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
static ssize_t dev_write_timeout(struct file *, const char *, size_t, loff_t *);
static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off);
static ssize_t dev_read_timeout(struct file *filp, char *buff, size_t len, 
								loff_t *off);
static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static int dev_flush(struct file *filp, void *id);

// This will hold the messages in a queue
struct queue_elem {
	char *message;
	unsigned long mess_len;
	struct lf_queue_node list;
};

struct session_data {
	ktime_t send_timeout, recv_timeout;
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
static DEFINE_LF_QUEUE(queue);
static DECLARE_WAIT_QUEUE_HEAD(wait_queue); // replace with init_waitqueue_head to init dynamically
static long max_message_size = 512;
static long max_storage_size = 4096;
static atomic_t stored_bytes = {.counter = 0};

// Module parameters exposed via the /sys/ pseudo-fs
module_param(max_message_size, long, 0660);
module_param(max_storage_size, long, 0660);

// Driver implementation
static int dev_open(struct inode *inode, struct file *filp) {
	// Increment usage count if there's any IO session towards files managed 
	// by this module
	if (!try_module_get(THIS_MODULE)) {
		return -ENODEV; // Module might have been removed
	}

	printk("%s: device file opened\n", MODNAME);
	return 0;
}

static int dev_release(struct inode *inode, struct file *filp) {
	module_put(THIS_MODULE);

	// private_data could contain data about the session, free it
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
	elem = vmalloc(sizeof(struct queue_elem));
	if (elem != NULL)
		elem->message = vmalloc(len);
	
	if (elem == NULL || elem->message == NULL) {
		// Allocations failed, exit with an error
		if (elem != NULL) vfree(elem);

		retval = -ENOMEM;
		goto exit;
	}

	printk("	%lu bytes to write", len);
	failed = copy_from_user(elem->message, buff, len);
	retval = elem->mess_len = len - failed;
	lf_queue_push(&queue, (&(elem->list)));
	atomic_add(retval, &stored_bytes); // Keep track of used space in device

	// TODO: call wake_up on the wait queue to wake up one thread
	
	exit:
	return retval;
}

static ssize_t __read_common(struct file *filp, char *buff, size_t len, 
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

static ssize_t dev_read(struct file *filp, char *buff, size_t len, 
						loff_t *off) {
	
	return __read_common(filp, buff, len, off);
}

static ssize_t dev_read_timeout(struct file *filp, char *buff, size_t len, 
								loff_t *off) {
	int wait_ret = 0;
	ssize_t retval = 0;
	ktime_t entry_time, wakeup_time, timeout;

	entry_time = wakeup_time = ktime_get();

	retry:
	// Compute remaining time to wait in case multiple wake-ups happen
	timeout = ((struct session_data *) filp->private_data)->recv_timeout - 
					ktime_sub(wakeup_time, entry_time);
	
	if (timeout < 0) {
		// Time's up, return now
		retval = -ETIME;
		goto exit;
	}

	if (queue.head == NULL) // 
		wait_ret = wait_event_hrtimeout(wait_queue, (queue.head != NULL), 
										timeout);

	if (wait_ret == 0) {
		// Condition has become true, try to read
		wakeup_time = ktime_get();
		retval = __read_common(filp, buff, len, off);

		if (retval != 0)
			goto exit; // Success, return
		else
			goto retry; // Someone else stole the message, try to sleep again
		
	} else if (wait_ret == -ETIME) {
		// Time's up, return now
		retval = -ETIME;
		goto exit;
	}

	exit:
	return retval;
}

static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	long retval = 0;
	void *res;
	struct session_data *s_data;

	printk("%s: called with cmd %u and arg %lu", MODNAME, cmd, arg);

	if (cmd == SET_SEND_TIMEOUT || cmd == SET_RECV_TIMEOUT) {
		retry:
		if (filp->private_data == NULL) {
			s_data = vmalloc(sizeof(struct session_data));

			if (s_data != NULL) {
				s_data->queue = NEW_LF_QUEUE;
				
				// try an atomic CAS on filp->private_data to set it to s_data,  
				// if it fails dealloc and go to retry just to be sure
				res = __sync_val_compare_and_swap(&(filp->private_data), 
												NULL, (void *) s_data);
				if (res != NULL) {
					vfree(s_data);
					goto retry;
				}
			} else {
				retval = -ENOMEM;
				goto exit;
			}
		}
	}

	s_data = filp->private_data;

	switch (cmd) {
	case SET_SEND_TIMEOUT:
		s_data->send_timeout = (ktime_t) arg; // Timeout in nanoseconds
		__sync_synchronize(); // Memory barrier

		// TODO: find a way to make this timeout a real thing
		break;
	case SET_RECV_TIMEOUT:
		s_data->recv_timeout = (ktime_t) arg; // Timeout in nanoseconds
		__sync_synchronize(); // Memory barrier

		// TODO: find a way to make this timeout a real thing
		// Wait on a wait_queue unique to the file with wait_event_hrtimeout
		// a wake_up call on the queue wakes up a single thread
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
	.owner = THIS_MODULE,
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
