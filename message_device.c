#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "libs/lf_queue/lf_queue.h"

MODULE_AUTHOR("Eduard Manta");
MODULE_LICENSE("GPL");
#define EXPORT_SYMTAB
#define MODNAME "Messages system"
#define DEVICE_NAME "message-dev"



#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
	#define get_major(session) MAJOR(session->f_inode->i_rdev)
	#define get_minor(session) MINOR(session->f_inode->i_rdev)
	#else
	#define get_major(session) MAJOR(session->f_dentry->d_inode->i_rdev)
	#define get_minor(session) MINOR(session->f_dentry->d_inode->i_rdev)
#endif


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


// Struct definitions

// This will hold the messages in a queue
struct queue_elem {
	char *message;
	unsigned long mess_len;
	struct lf_queue_node list;
};

// Data related to a single I/O session, will be stored in the private_data 
// pointer of the file struct
struct session_data {
	ktime_t send_timeout, recv_timeout;
	struct lf_queue queue;
	struct mutex metadata_lock;
};

// ioctl commands
enum ioctl_cmds {
	SET_SEND_TIMEOUT = 99,
	SET_RECV_TIMEOUT,
	REVOKE_DELAYED_MESSAGES
};

// Globals
static int MAJOR;
static long max_message_size = 512;
static long max_storage_size = 4096;
// TODO: move these (vvv) to a struct and make one for each minor
static DEFINE_LF_QUEUE(queue);
static DECLARE_WAIT_QUEUE_HEAD(wait_queue); // replace with init_waitqueue_head to init dynamically
static atomic_t stored_bytes = {.counter = 0};

// Module parameters exposed via the /sys/ pseudo-fs
module_param(max_message_size, long, 0660);
module_param(max_storage_size, long, 0660);

// Macro to create a struct containing a driver instance
#define DEFINE_DRIVER_INSTANCE(read_fp, write_fp) ((struct file_operations) { \
	.owner = THIS_MODULE, \
	.write = write_fp, \
	.read = read_fp, \
	.open =  dev_open, \
	.release = dev_release, \
	.unlocked_ioctl = dev_ioctl, \
	.flush = dev_flush \
})

// A table that holds all the possible drivers; when adding a timeout for reads 
// or writes the f_op in the file struct will be changed to point to one of 
// these driver instances
static struct file_operations f_ops[4] = {
	// No-timeout read and write
	DEFINE_DRIVER_INSTANCE(dev_read, dev_write),
	// Read with timeout, normal write
	DEFINE_DRIVER_INSTANCE(dev_read_timeout, dev_write),
	// Normal read, delayed write
		//DEFINE_DRIVER_INSTANCE(dev_read, dev_write_timeout),
	DEFINE_DRIVER_INSTANCE(dev_read, NULL), // TODO: Disable and use line above
	// Read with timeout, delayed write
		//DEFINE_DRIVER_INSTANCE(dev_read_timeout, dev_write_timeout),
	DEFINE_DRIVER_INSTANCE(dev_read_timeout, NULL) // TODO: Disable and use line above
};

// Possibile indices for f_ops table
enum driver_mode {
	R_W = 0,	// No-timeout read and write
	Rt_W,		// Read with timeout, normal write
	R_Wt,		// Normal read, delayed write
	Rt_Wt		// Read with timeout, delayed write
};

// Get the index of the correct driver based on send and receive timeout values
#define DRIVER_INDEX(st, rt) ((enum driver_mode) ((rt ? 0x1 : 0) | (st ? 0x2 : 0)))


// Driver implementation

// Opens an I/O session towards the device
static int dev_open(struct inode *inode, struct file *filp) {
	// TODO: Check minor number, if too big return -ENODEV

	// Increment usage count if there's any IO session towards files managed 
	// by this module
	if (!try_module_get(THIS_MODULE)) {
		return -ENODEV; // Module might have been removed
	}

	printk("%s: device file opened\n", MODNAME);
	return 0;
}

// Closes an I/O session towards the device
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

// Posts a message on the message queue
static ssize_t dev_write(struct file *filp, const char *buff, size_t len, 
						loff_t *off) {
	struct queue_elem *elem;
	unsigned long failed;
	ssize_t retval;

	printk("%s: write on [%d,%d]\n", MODNAME, get_major(filp), get_minor(filp));

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

	wake_up(&wait_queue); // Wake up one thread on the wait queue
	
	exit:
	return retval;
}

// Reads a message (or a part of one) from the message queue; common 
// implementation called by all the exposed read variants
static ssize_t __read_common(struct file *filp, char *buff, size_t len, 
							loff_t *off) {
	struct lf_queue_node *node;
	struct queue_elem *elem;
	ssize_t retval = 0;

	printk("%s: read on [%d,%d]\n", MODNAME, get_major(filp), get_minor(filp));
	
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

// Read a message from the queue; if none is available returns immediately
static ssize_t dev_read(struct file *filp, char *buff, size_t len, 
						loff_t *off) {
	
	return __read_common(filp, buff, len, off);
}

// Read a message from the queue; if none is available wait for a time related 
// to the I/O session
// Threads will wait on a wait_queue dedicated to the file using 
// wait_event_hrtimeout; a wake_up call on the queue will wake up a single 
// thread
static ssize_t dev_read_timeout(struct file *filp, char *buff, size_t len, 
								loff_t *off) {
	int wait_ret = 0;
	ssize_t retval = 0;
	ktime_t entry_time, wakeup_time, timeout;

	entry_time = wakeup_time = ktime_get();
	timeout = ((struct session_data *) filp->private_data)->recv_timeout;

	printk("%s: read with timeout %lldns", MODNAME, timeout);

	retry:
	// Compute remaining time to wait in case multiple wake-ups happen:
	// timeout = timeout - (wakeup_time - entry_time)
	timeout = ktime_sub(timeout, ktime_sub(wakeup_time, entry_time));
	
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

// Allocate a session_data struct for the specified I/O session in a 
// concurrency-safe fashion
static long __alloc_session_data(struct file *filp) {
	struct session_data *s_data;
	void *res;
	long retval = 0;
	
	retry:
	if (filp->private_data == NULL) {
		s_data = vmalloc(sizeof(struct session_data));

		if (s_data != NULL) {
			s_data->queue = NEW_LF_QUEUE;
			s_data->recv_timeout = s_data->send_timeout = ktime_set(0, 0);
			mutex_init(&(s_data->metadata_lock));
			
			// try an atomic CAS on filp->private_data to set it to s_data,  
			// if it fails dealloc and go to retry just to be sure
			res = __sync_val_compare_and_swap(&(filp->private_data), 
											NULL, (void *) s_data);
			if (res != NULL) {
				vfree(s_data);
				goto retry;
			} else goto exit;
		} else {
			retval = -ENOMEM;
			goto exit;
		}
	}

	exit:
	return retval;
}

// Changes the device operation mode according to cmd and, if necessary, the 
// value of arg
static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	long retval = 0;
	struct session_data *s_data;

	printk("%s: called with cmd %u and arg %lu", MODNAME, cmd, arg);

	if (filp->private_data == NULL) {
		retval = __alloc_session_data(filp);
		if (retval != 0)
			goto exit;
	}

	s_data = (struct session_data *) filp->private_data;

	switch (cmd) {
	case SET_SEND_TIMEOUT:
	case SET_RECV_TIMEOUT:
		mutex_lock(&(s_data->metadata_lock));

		if (cmd == SET_SEND_TIMEOUT)
			s_data->send_timeout = ktime_set(0, arg);
		else
			s_data->recv_timeout = ktime_set(0, arg);
		
		// Select correct driver
		filp->f_op = &f_ops[DRIVER_INDEX(s_data->send_timeout, s_data->recv_timeout)];

		mutex_unlock(&(s_data->metadata_lock));
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

int init_module(void) {
	MAJOR = __register_chrdev(0, 0, 256, DEVICE_NAME, &f_ops[R_W]);

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
