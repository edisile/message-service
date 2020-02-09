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
#include "libs/ring_bitmask/ring_bitmask.h"

MODULE_AUTHOR("Eduard Manta");
MODULE_LICENSE("GPL");
#define EXPORT_SYMTAB
#define MODNAME "Messages system"
#define DEVICE_NAME "message-dev"



#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
	#define __MINOR(session) MINOR(session->f_inode->i_rdev)
	#else
	#define __MINOR(session) MINOR(session->f_dentry->d_inode->i_rdev)
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
	unsigned char *message;
	unsigned long mess_len;
	struct lf_queue_node list;
};

// Data related to a single I/O session, will be stored in the private_data 
// pointer of the file struct
struct session_data {
	unsigned long send_timeout;
	ktime_t recv_timeout;
	struct lf_queue queue;
	struct mutex metadata_lock;
	long delay_status_ind; // Index of a delayed write status
};

// Data related to a single instance of the file
struct file_data {
	struct lf_queue message_queue;
	atomic_t stored_bytes;
	struct wait_queue_head wait_queue;
	struct ring_bitmask delays; // Will store the status of delayed writes
};

// Used to store data necessary for the delayed write mechanism
struct delayed_write_data {
	struct delayed_work dwork;
	struct queue_elem *elem;
	struct file_data *file;
	long delay_status_ind;
};

// ioctl commands
enum ioctl_cmds {
	SET_SEND_TIMEOUT = 99,
	SET_RECV_TIMEOUT,
	REVOKE_DELAYED_MESSAGES
};

// Globals
static int MAJOR;
#ifndef MINORS
	#define MINORS 64
#endif
static long max_message_size = 512;
static long max_storage_size = 4096;
static struct file_data files[MINORS];
static struct workqueue_struct *work_queue;

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
	DEFINE_DRIVER_INSTANCE(dev_read, dev_write_timeout),
	// Read with timeout, delayed write
	DEFINE_DRIVER_INSTANCE(dev_read_timeout, dev_write_timeout)
};

// Get the index of the correct driver based on send and receive timeout values
#define DRIVER_INDEX(st, rt) (((rt ? 0x1 : 0) | (st ? 0x2 : 0)))


// Driver implementation

// Opens an I/O session towards the device
static int dev_open(struct inode *inode, struct file *filp) {
	int retval = 0;

	// if (__MINOR(filp) >= MINORS) {
	// 	// Minor number not supported
	// 	retval = -ENODEV;
	// 	goto exit;
	// }

	// MAYBE: remove
	// Increment usage count if there's any IO session towards files managed 
	// by this module
	if (!try_module_get(THIS_MODULE)) {
		// Module might have been removed
		retval = -ENODEV;
		goto exit;
	}

	printk("%s: device file opened\n", MODNAME);

	exit:
	return retval;
}

// Closes an I/O session towards the device
static int dev_release(struct inode *inode, struct file *filp) {
	module_put(THIS_MODULE); // MAYBE: remove

	// 
	// private_data could contain data about the session, free it
	if (filp->private_data != NULL) {
		vfree(filp->private_data);
		filp->private_data = NULL;
	}

	printk("%s: device file closed\n", MODNAME);
	return 0;
}

// Stores len bytes (or less) of the message provided in buff inside a 
// queue_elem whose address is placed at elem_addr
static ssize_t __write_common(struct file_data *d, const char *buff, size_t len, 
							struct queue_elem **elem_addr) {
	ssize_t retval;
	unsigned long failed;
	
	if (len > min(max_message_size, max_storage_size - d->stored_bytes.counter)) {
		retval = -ENOSPC;
		goto exit;
	}

	// Allocate memory for the message
	*elem_addr = vmalloc(sizeof(struct queue_elem));
	if (elem_addr != NULL)
		(*elem_addr)->message = vmalloc(len);
	
	if (*elem_addr == NULL || (*elem_addr)->message == NULL) {
		// Allocations failed, exit with an error
		if (*elem_addr != NULL) vfree(*elem_addr);

		retval = -ENOMEM;
		goto exit;
	}

	printk("	%lu bytes to write", len);
	failed = copy_from_user((*elem_addr)->message, buff, len);
	retval = (*elem_addr)->mess_len = len - failed;
	atomic_add(retval, &(d->stored_bytes)); // Keep track of used space in device

	exit:
	return retval;
}

// Common implementation for pushing messages to the queue of a file
static void inline __push_to_queue(struct file_data *d, struct queue_elem *e) {
	lf_queue_push(&(d->message_queue), &(e->list));
	wake_up(&(d->wait_queue)); // Wake up one thread on the wait queue
}

// Posts a message on the message queue
static ssize_t dev_write(struct file *filp, const char *buff, size_t len, 
						loff_t *off) {
	struct file_data *d = &files[__MINOR(filp)];
	struct queue_elem *elem;
	ssize_t retval;

	printk("%s: write on [%d,%d]\n", MODNAME, MAJOR, __MINOR(filp));

	retval = __write_common(d, buff, len, &elem);
	if (retval == -ENOMEM || retval == -ENOSPC) goto exit;

	__push_to_queue(d, elem);
	
	exit:
	return retval;
}

// Function to be executed as deferred work in order to push a message to the 
// queue; before pushing, it checks if the push has been aborted by an ioctl 
// REVOKE_DELAYED_MESSAGES request or a flush
static void __delayed_work(struct work_struct *work) {
	struct delayed_work *d_work;
	struct delayed_write_data *data;

	d_work = container_of(work, struct delayed_work, work);
	data = container_of(d_work, struct delayed_write_data, dwork);

	printk("%s: delayed func call\n", MODNAME);

	// Check if push was aborted in the meantime
	if (is_enabled(&(data->file->delays), data->delay_status_ind)) {
		__push_to_queue(data->file, data->elem);
		printk("%s: delayed push success\n", MODNAME);
	} else {
		printk("%s: delayed push aborted\n", MODNAME);
		vfree(data->elem->message);
		vfree(data->elem);
	}

	vfree(data); 
}

// Schedules a delayed work item to post a message on the queue; the message 
// can be revoked via a REVOKE_DELAYED_MESSAGES ioctl call or a flush
static ssize_t dev_write_timeout(struct file *filp, const char *buff, 
								size_t len, loff_t *off) {
	struct file_data *d = &files[__MINOR(filp)];
	struct session_data *s_data =  (struct session_data *) filp->private_data;
	struct queue_elem *elem;
	struct delayed_write_data *data = NULL;
	bool ok;
	ssize_t retval;

	printk("%s: delayed write on [%d,%d]\n", MODNAME, MAJOR, __MINOR(filp));

	retval = __write_common(d, buff, len, &elem);
	if (retval == -ENOMEM || retval == -ENOSPC) goto exit;

	data = vmalloc(sizeof(struct delayed_write_data));
	if (data == NULL) {
		retval = -ENOMEM;
		goto cleanup;
	}

	data->elem = elem;
	data->file = d;

	if (s_data->delay_status_ind == -1 || 
		is_disabled(&(d->delays), s_data->delay_status_ind)) {
		// Delayed push is not enabled for this elem anymore, get a new one
		printk("%s: requesting new delay status\n", MODNAME);
		s_data->delay_status_ind = get_free(&(d->delays));

		if (s_data->delay_status_ind == -1) {
			retval = -ENOMEM;

		}
	}

	data->delay_status_ind = s_data->delay_status_ind;
	INIT_DELAYED_WORK(&(data->dwork), __delayed_work);

	ok = queue_delayed_work(work_queue, &(data->dwork), s_data->send_timeout);
	if (!ok) {
		retval = -EAGAIN;
		goto cleanup;
	}
	printk("%s: work enqueued, 0x%p\n", MODNAME, data);

	exit:
	return retval;

	cleanup:
	if (data != NULL) vfree(data);
	vfree(elem->message);
	vfree(elem);
	return retval;
}

// Reads a message (or a part of one) from the message queue; common 
// implementation called by all the exposed read variants
static ssize_t __read_common(struct file_data *d, char *buff, size_t len, 
							loff_t *off) {
	struct lf_queue_node *node;
	struct queue_elem *elem;
	ssize_t retval = 0;

	printk("%s: read on [%d,%d]\n", MODNAME, MAJOR, (int) (d - files));
	
	node = lf_queue_pull(&(d->message_queue));

	if (node != NULL) {
		elem = container_of(node, struct queue_elem, list);
		retval = len = min(len, elem->mess_len);
		printk("	%lu bytes to read", len);

		if (buff != NULL) // buff == NULL means just flushing the message
			copy_to_user(buff, elem->message + *off, len);
	
		atomic_sub(elem->mess_len, &(d->stored_bytes)); // Space being freed
		vfree(elem->message);
		vfree(elem);
	}

	return retval;
}

// Read a message from the queue; if none is available returns immediately
static ssize_t dev_read(struct file *filp, char *buff, size_t len, 
						loff_t *off) {

	return __read_common(&files[__MINOR(filp)], buff, len, off);
}

// Read a message from the queue; if none is available wait for a time related 
// to the I/O session
// Threads will wait on a wait_queue dedicated to the file using 
// wait_event_hrtimeout; a wake_up call on the queue will wake up a single 
// thread every time a new message is added
static ssize_t dev_read_timeout(struct file *filp, char *buff, size_t len, 
								loff_t *off) {
	struct file_data *d = &files[__MINOR(filp)];
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

	if (d->message_queue.head == NULL) {
		wait_ret = wait_event_hrtimeout(d->wait_queue, 
										d->message_queue.head != NULL, timeout);
	}

	if (wait_ret == 0) {
		// Condition has become true, try to read
		wakeup_time = ktime_get();
		retval = __read_common(d, buff, len, off);

		if (retval > 0)
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
	struct file_data *d = &files[__MINOR(filp)];

	printk("%s: called with cmd %u and arg %lu", MODNAME, cmd, arg);

	if (filp->private_data == NULL) {
		retval = __alloc_session_data(filp);
		if (retval != 0) {
			printk("%s: ioctl failed", MODNAME);
			goto exit;
		}
	} 

	s_data = (struct session_data *) filp->private_data;

	switch (cmd) {
	case SET_SEND_TIMEOUT:
	case SET_RECV_TIMEOUT:
		mutex_lock(&(s_data->metadata_lock));

		if (cmd == SET_SEND_TIMEOUT)
			s_data->send_timeout = arg;
		else
			s_data->recv_timeout = ktime_set(0, arg);
		
		// Select correct driver
		filp->f_op = &f_ops[DRIVER_INDEX(s_data->send_timeout, s_data->recv_timeout)];

		mutex_unlock(&(s_data->metadata_lock));
		break;
	case REVOKE_DELAYED_MESSAGES:
		// Set the delayed messages not yet pushed to the queue to be dumped
		put_used(&(d->delays), s_data->delay_status_ind);
		break;
	default:
		retval = -EINVAL;
		goto exit;
	}

	exit:
	return retval;
}

// Real flush implementation, just set all the delayed operations' status to 
// disabled; when they will execute, their messages won't be pushed to the queue
static void __flush(struct file_data *d) {
	wake_up_all(&(d->wait_queue));
	put_all(&(d->delays));
}

// Revoke all messages not yet pushed to the queue and wake up all waiting 
// readers; the exported VFS operation is just an wrapper for __flush
static int dev_flush(struct file *filp, void *id) {
	printk("%s: flush requested on [%d,%d]", MODNAME, MAJOR, __MINOR(filp));
	__flush(&files[__MINOR(filp)]);

	return 0;
}

int init_module(void) {
	int retval = 0;
	int i;
	// By default the driver for the device is f_ops[0] that points to the 
	// non-delayed read / write implementations
	struct file_operations *f_op = &f_ops[0];

	MAJOR = __register_chrdev(0, 0, MINORS, DEVICE_NAME, f_op);

	if (MAJOR < 0) {
		printk(KERN_ERR "%s: register failed with major %d\n", MODNAME, MAJOR);
		retval = MAJOR;
		goto exit;
	}

	printk("%s: registered as %d\n", MODNAME, MAJOR);

	work_queue = alloc_workqueue("delayed_queue", WQ_UNBOUND, 0);
	if (work_queue == NULL) {
		// Work queue was not allocated due to kzalloc failure, return with a 
		// "no memory" error
		retval = -ENOMEM;
		goto exit;
	}

	// Initialize the data struct for all possible file instances
	for (i = 0; i < MINORS; i++) {
		files[i] = (struct file_data) {
			.message_queue = NEW_LF_QUEUE,
			.stored_bytes = ATOMIC_INIT(0),
			.delays = NEW_RING_BITMASK
		};

		init_waitqueue_head(&(files[i].wait_queue));
	}

	exit:
	return retval;
}

void cleanup_module(void) {
	int i;

	// Flush all files; sets all delayed work to dump their messages and wakes 
	// up any readers that are still waiting
	for (i = 0; i < MINORS; i++) {
		__flush(&(files[i]));
	}

	// Flush all work in the queue before destroying it
	printk("%s: flushing work queue\n", MODNAME);
	flush_workqueue(work_queue);
	destroy_workqueue(work_queue);

	__unregister_chrdev(MAJOR, 0, MINORS, DEVICE_NAME);
	printk("%s: unregistered\n", MODNAME);

	for (i = 0; i < MINORS; i++) {
		printk("%s: cleaning device #%d\n", MODNAME, i);
		// calling __read_common with buff == NULL just dumps the message from 
		// the queue
		while (__read_common(&files[i], NULL, 1, NULL) > 0);
	}

	return;
}
