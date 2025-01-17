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
#include "libs/atomic_long_kp/atomic_long_kp.h"
#include "libs/timestamp/timestamp.h"
#include "libs/ordered_wait_queue/ordered_wait_queue.h"
#include "libs/hr_work_queue/hr_work_queue.h"



MODULE_AUTHOR("Eduard Manta");
MODULE_LICENSE("GPL");
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
struct mq_message {
	unsigned char *message;
	unsigned long mess_len;
	struct lf_queue_node list;
};

// Data related to a single I/O session, will be stored in the private_data 
// pointer of the file struct
struct session_data {
	ktime_t send_timeout;
	ktime_t recv_timeout;
	struct mutex metadata_lock;
	struct timestamp *ts;
	atomic_t refs;
};

// Data related to a single instance of the file
struct file_data {
	struct lf_queue message_queue;
	atomic_long_t stored_bytes;
	struct ordered_wait_queue wait_queue;
	struct hr_work_queue work_queue;
	ktime_t flush_time;
};

// Used to store data necessary for the delayed write mechanism
struct delayed_write_data {
	struct hr_work hrwork;
	struct mq_message *elem;
	struct file_data *file;
	struct timestamp *ts;
	ktime_t post_time;
};

#define dwd_from_work(_work) (container_of(_work, struct delayed_write_data, \
											hrwork))



// ioctl commands
#define IOCTL_CODE 0x27 // 0x2700 = 9984 as base
#define SET_SEND_TIMEOUT _IOW(IOCTL_CODE, 0x0f, long) // 9984 + 15 = 9999
#define SET_RECV_TIMEOUT _IOW(IOCTL_CODE, 0x10, long) // 9984 + 16 = 10000
#define REVOKE_DELAYED_MESSAGES _IO(IOCTL_CODE, 0x11) // 9984 + 17 = 10001


// Globals
static int MAJOR;
#ifndef MINORS
	#define MINORS 4
#endif
static atomic_long_t max_message_size = ATOMIC_LONG_INIT(1 << 10); // 1KiB
static atomic_long_t max_storage_size = ATOMIC_LONG_INIT(1 << 22); // 4MiB
static struct file_data files[MINORS];


// Module parameters exposed via the /sys/ pseudo-fs; atomic_long_param_ops is 
// a custom kernel_param_ops struct that implements a atomic set on variables 
// of atomic_long_t type
module_param_cb(max_message_size, &atomic_long_param_ops, &max_message_size, 
				0664);
module_param_cb(max_storage_size, &atomic_long_param_ops, &max_storage_size, 
				0664);


// Macro to create a struct containing a driver instance
#define DEFINE_DRIVER_INSTANCE(read_fp, write_fp) ((struct file_operations) {	\
	.owner = THIS_MODULE,														\
	.write = write_fp,															\
	.read = read_fp,															\
	.open =  dev_open,															\
	.release = dev_release,														\
	.unlocked_ioctl = dev_ioctl,												\
	.flush = dev_flush															\
})

// A table that holds all the possible drivers; when adding a timeout for reads 
// or writes the f_op pointer in the file struct will be changed to point to 
// one of these driver instances
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
#define DRIVER_INDEX(st, rt) ((rt ? 0x1 : 0) | (st ? 0x2 : 0))


// Helper functions for acquiring and releasing references to a session_data 
// struct

// Converts a void pointer like void *file::private_data to an s_data pointer 
// while counting references to this struct
static inline struct session_data *__acquire_s_data(void *ptr) {
	struct session_data *s_data = NULL;

	s_data = (struct session_data *) ptr;
	if (s_data != NULL) {
		atomic_inc(&(s_data->refs));
	}
	
	return s_data;
}

// Decrements the reference counter for the instance of s_data struct and frees 
// it when there's no more references
static inline void __release_s_data(struct session_data *s_data) {
	if (s_data != NULL)
		if (atomic_dec_and_test(&s_data->refs))
			vfree(s_data);
}


// Driver implementation

// Opens an I/O session towards the device
static int dev_open(struct inode *inode, struct file *filp) {
	printk("%s: device file opened\n", MODNAME);
	return 0;
}

// Closes an I/O session towards the device
static int dev_release(struct inode *inode, struct file *filp) {
	struct session_data *s_data = (struct session_data *) filp->private_data;

	// s_data might have not been created for this structure
	if (s_data != NULL) {
		// Hide the session_data struct from any new accessors
		atomic_long_set((atomic_long_t *) filp->private_data, (long) NULL);
	
		__release_timestamp(s_data->ts);

		// Wait for the references to drop to 1
		while (s_data->refs.counter > 1)
			usleep_range(50, 100);

		__release_s_data(s_data);
	}

	printk("%s: device file closed\n", MODNAME);
	return 0;
}

// Stores the message provided in buff inside a mq_message whose address is 
// placed at elem_addr while keeping the count of the bytes stored in the device
static ssize_t __write_common(struct file_data *d, const char *buff, size_t len, 
							struct mq_message **elem_addr) {
	unsigned char *message;
	long free_b, stored_b;
	ssize_t retval;
	
	if (len > atomic_long_read(&max_message_size)) {
		printk("%s: message too big; %luB message, %ldB max size\n", MODNAME, 
				len, atomic_long_read(&max_message_size));
		return -EMSGSIZE;
	}

	retry:
	// Calculate free space on device, if message is too large return -ENOSPC, 
	// otherwise try to pre-reserve len bytes
	stored_b = atomic_long_read(&(d->stored_bytes));
	free_b = atomic_long_read(&max_storage_size) - stored_b;
	
	if (len > free_b) {
		printk("%s: message too big; %luB message, %ldB available space\n", 
				MODNAME, len, free_b);
		return -ENOSPC;
	}
	if (atomic_long_cmpxchg(&(d->stored_bytes), stored_b, stored_b + len) != 
			stored_b)
		goto retry;

	// Allocate memory for the message
	*elem_addr = vmalloc(sizeof(struct mq_message));
	message = vmalloc(len);

	if (*elem_addr == NULL || message == NULL) {
		// Allocations failed, exit with an error
		retval = -ENOMEM;
		goto cleanup;
	}

	printk("%s:	%lu bytes to write", MODNAME, len);
	if (copy_from_user(message, buff, len) != 0) {
		printk("%s:	failure to copy all bytes, aborting write", MODNAME);
		retval = -EFAULT;
		goto cleanup;
	}

	(*elem_addr)->message = message;
	retval = (*elem_addr)->mess_len = len;

	return retval;

	cleanup:
	if (*elem_addr != NULL) vfree(*elem_addr);
	if (message != NULL) vfree(message);
	// Message posting failed, free the pre-reserved space
	atomic_long_sub(len, &(d->stored_bytes));
	return retval;
}

// Common implementation for pushing messages to the queue of a file
static void inline __push_to_queue(struct file_data *d, struct mq_message *e) {
	lf_queue_push(&(d->message_queue), &(e->list));
	wake_up_ordered(&(d->wait_queue)); // Wake up one thread on the wait queue
}

// Posts a message on the message queue
static ssize_t dev_write(struct file *filp, const char *buff, size_t len, 
						loff_t *off) {
	struct file_data *d = &files[__MINOR(filp)];
	struct mq_message *elem;
	ssize_t retval;

	printk("%s: write on [%d,%d]\n", MODNAME, MAJOR, __MINOR(filp));

	retval = __write_common(d, buff, len, &elem);

	// Check if message storing failed for any reason
	switch (retval) {
	case -ENOMEM:
	case -ENOSPC:
	case -EMSGSIZE:
	case -EFAULT:
		break;
	default:
		__push_to_queue(d, elem);
	}

	return retval;
}

// Function to be executed as deferred work in order to push a message to the 
// queue; before pushing, it checks if the push has been aborted by an ioctl 
// REVOKE_DELAYED_MESSAGES request or a flush
static void __delayed_work(struct work_struct *work) {
	struct delayed_write_data *data;

	data = container_of(work, struct delayed_write_data, hrwork.work);

	printk("%s: delayed func call\n", MODNAME);

	if (ktime_before(data->post_time, data->file->flush_time)) {
		printk("%s: push should be aborted because of flush", MODNAME);
	}

	if (ktime_before(data->post_time, data->ts->time)) {
		printk("%s: push should be aborted because of revoke", MODNAME);
	}

	// Check if push was aborted in the meantime: if the message timestamp 
	// comes after the last flush and last revoke push it to the queue
	if (ktime_after(data->post_time, data->file->flush_time) &&
			ktime_after(data->post_time, data->ts->time)) {
		__push_to_queue(data->file, data->elem);
		printk("%s: delayed push success\n", MODNAME);
	} else {
		printk("%s: delayed push aborted\n", MODNAME);
		// Free the message and remove if from the stored bytes count
		atomic_long_sub(data->elem->mess_len, &(data->file->stored_bytes));
		vfree(data->elem->message);
		vfree(data->elem);
	}

	__release_timestamp(data->ts);
	vfree(data); 
}

// Schedules a delayed work item to post a message on the queue; the message 
// can be revoked via a REVOKE_DELAYED_MESSAGES ioctl call or a flush
static ssize_t dev_write_timeout(struct file *filp, const char *buff, 
								size_t len, loff_t *off) {
	struct file_data *d = &files[__MINOR(filp)];
	struct session_data *s_data;
	struct mq_message *elem;
	struct delayed_write_data *data = NULL;
	ktime_t curr_time = ktime_get();
	bool ok;
	ssize_t retval;

	printk("%s: delayed write on [%d,%d]\n", MODNAME, MAJOR, __MINOR(filp));

	s_data = __acquire_s_data(filp->private_data);
	if (s_data == NULL) {
		// s_data has been hidden/deallocated while closing the session
		printk("%s: ts access from NULL s_data pointer", MODNAME);
		retval = -EBADFD;
		goto exit;
	}

	retval = __write_common(d, buff, len, &elem);
	if (retval == -ENOMEM || retval == -ENOSPC)
		goto exit; // Write failed to store the message
	
	data = vmalloc(sizeof(struct delayed_write_data));
	if (data == NULL) {
		retval = -ENOMEM;
		goto cleanup;
	}

	data->elem = elem;
	data->file = d;
	// The timestamp will be referenced by another delayed work, acquire it
	data->ts = __acquire_timestamp(s_data->ts);
	data->post_time = curr_time;

	INIT_HR_WORK_REL(data->hrwork, __delayed_work, s_data->send_timeout);
	ok = queue_hr_work(&(d->work_queue), &(data->hrwork));

	if (!ok) {
		__release_timestamp(s_data->ts); // The ts reference is no more
		retval = -EAGAIN;
		goto cleanup;
	}

	printk("%s: work enqueued, 0x%p\n", MODNAME, data);

	exit:
	__release_s_data(s_data);
	return retval;

	cleanup:
	if (data != NULL) vfree(data);
	__release_s_data(s_data);
	vfree(elem->message);
	vfree(elem);
	return retval;
}

// Reads a message (or a part of one) from the message queue; common 
// implementation called by all the exposed read variants
static ssize_t __read_common(struct file_data *d, char *buff, size_t len, 
							loff_t *off) {
	struct lf_queue_node *node;
	struct mq_message *elem;
	ssize_t retval = 0;

	printk("%s: read on [%d,%d]\n", MODNAME, MAJOR, (int) (d - files));
	
	node = lf_queue_pull(&(d->message_queue));

	if (node != NULL) {
		elem = container_of(node, struct mq_message, list);

		retval = len = min(len, elem->mess_len);
		// buff == NULL means just flushing the message, useful when unmounting 
		// the module in order to clean up
		if (buff != NULL) {
			printk("	%lu bytes to read", len);
			copy_to_user(buff, elem->message + *off, len);
		}

		// Free the message and subtract it from the stored bytes count
		atomic_long_sub(elem->mess_len, &(d->stored_bytes));
		vfree(elem->message);
		vfree(elem);
	}

	return retval;
}

// Read a message from the queue; if none is available returns immediately
static ssize_t dev_read(struct file *filp, char *buff, size_t len, 
						loff_t *off) {
	
	if (buff != NULL)
		return __read_common(&files[__MINOR(filp)], buff, len, off);
	else
		return -EINVAL;
}

// Read a message from the queue; if none is available wait for a time related 
// to the I/O session
// Threads will wait on a ordered_wait_queue dedicated to the file using 
// wait_event_hrtimeout; a wake_up call on the queue will wake up a single 
// thread every time a new message is added
static ssize_t dev_read_timeout(struct file *filp, char *buff, size_t len, 
								loff_t *off) {
	struct file_data *d = &files[__MINOR(filp)];
	struct session_data *s_data;
	int wait_ret = 0;
	ssize_t retval = 0;
	ktime_t entry_time, wakeup_time, timeout;
	bool flushed;

	s_data = __acquire_s_data(filp->private_data);
	if (s_data == NULL) {
		// Someone closed the file concurrently to the read call
		printk("%s: NULL s_data pointer", MODNAME);
		return -EBADFD;
	}

	entry_time = wakeup_time = ktime_get();
	timeout = s_data->recv_timeout;

	printk("%s: thread %d read with timeout %lldns", MODNAME, current->pid, 
			timeout);

	retry:
	// Compute remaining time to wait in case multiple wake-ups happen:
	// timeout = timeout - (wakeup_time - entry_time)
	timeout = ktime_sub(timeout, ktime_sub(wakeup_time, entry_time));

	if (timeout < 0) {
		// Time's up, return now
		printk("%s: timer expired", MODNAME);
		retval = -ETIME;
		goto exit;
	}

	if (IS_EMPTY(d->message_queue)) {
		// Wait until either a signal comes...
		wait_ret = wait_event_ordered_interruptible_hrtimeout(&(d->wait_queue), 
				// ... queue is not empty...
				!IS_EMPTY(d->message_queue) ||
				// ... flush is requested...
				(flushed = ktime_after(d->flush_time, entry_time)),
				// ... or timeout expires
				entry_time, timeout);
	}

	wakeup_time = ktime_get();

	switch (wait_ret) {
	case 0:
		if (!flushed) {
			// Condition has become true, try to read
			retval = __read_common(d, buff, len, off);

			if (retval == 0)
				goto retry; // Someone stole the message, try to sleep again
		} else {
			printk("%s: woke up because of flush", MODNAME);
			retval = -EAGAIN;
		}
		break;
	case -ETIME:
	case -ERESTARTSYS:
		// Time's up or a signal was received, return now
		if (wait_ret == -ETIME)
			printk("%s: woke up because of timer", MODNAME);
		else
			printk("%s: woke up because of signal", MODNAME);
		
		retval = wait_ret;
		
		break;
	default:
		printk(KERN_ERR "%s: woke up with error %d; this shouldn't happen", 
				MODNAME, wait_ret);
		break;
	}

	exit:
	__release_s_data(s_data);
	return retval;
}

// Allocate a session_data struct for the specified I/O session in a 
// concurrency-safe fashion
static long __alloc_session_data(struct file *filp) {
	struct session_data *s_data;
	struct timestamp *ts;
	long retval = 0;
	
	retry:
	if (filp->private_data == NULL) {
		s_data = vmalloc(sizeof(struct session_data));
		ts = vmalloc(sizeof(struct timestamp));

		if (s_data != NULL && ts != NULL) {
			*ts = (struct timestamp) {
				.time = ktime_set(0, 0),
				.refs = ATOMIC_INIT(1)
			};

			*s_data = (struct session_data) {
				.ts = ts,
				.recv_timeout = ktime_set(0, 0),
				.send_timeout = 0,
				.refs = ATOMIC_INIT(1)
			};

			mutex_init(&(s_data->metadata_lock));
			
			// try an atomic CAS on filp->private_data to set it to s_data,  
			// if it fails dealloc and go to retry just to be sure
			retval = atomic_long_cmpxchg((atomic_long_t *) &(filp->private_data), 
										(long) NULL, (long) s_data);

			if ((void *) retval != NULL) {
				vfree(ts);
				vfree(s_data);
				goto retry;
			}
		} else {
			if (ts != NULL) vfree(ts);
			if (s_data != NULL) vfree(s_data);

			retval = -ENOMEM;
		}
	}

	return retval;
}

static inline void __revoke(struct timestamp *ts, struct hr_work_queue *wq) {
	struct hr_work *w;

	// Start all work items whose struct timestamp pointer is the one of the 
	// session and would have been valid at a time later than ts->time; works 
	// are built as to auto-deallocate themselves if this happens
	start_work_if(wq, w, (dwd_from_work(w)->ts == ts) && 
					ktime_after(w->time, ts->time));
}

// Changes the device operation mode according to cmd and, if necessary, the 
// value of arg
static long dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	long retval = 0;
	struct session_data *s_data;
	struct file_data *d = &files[__MINOR(filp)];
	struct task_struct *wq_daemon;

	printk("%s: called with cmd %u and arg %lu", MODNAME, cmd, arg);

	if (_IOC_TYPE(cmd) != IOCTL_CODE || 
			_IOC_NR(cmd) < _IOC_NR(SET_SEND_TIMEOUT) ||
			_IOC_NR(cmd) > _IOC_NR(REVOKE_DELAYED_MESSAGES)) {
		printk("%s: bad ioctl command", MODNAME);
		return -EINVAL;
	}

	// This session might not have an associated session_data struct yet
	if (filp->private_data == NULL) {
		retval = __alloc_session_data(filp);
		if (retval != 0) {
			printk("%s: ioctl failed", MODNAME);
			return retval;
		}
	}

	// Get a valid reference to the session_data struct
	s_data = __acquire_s_data(filp->private_data);
	if (s_data == NULL) {
		// Someone closed the file concurrently to the ioctl call
		printk("%s: ts access from NULL s_data pointer", MODNAME);
		retval = -EBADFD;
	}

	switch (_IOC_NR(cmd)) {
	case _IOC_NR(SET_SEND_TIMEOUT):
		// First to request a send timeout will start the work queue cloning a 
		// daemon thread
		if (!hr_work_queue_active(&(d->work_queue))) {
			printk("%s: trying to start hr_work_queue", MODNAME);
			wq_daemon = start_hr_work_queue(&(d->work_queue));

			if ((long) wq_daemon == -ENOMEM) {
				// forking the daemon failed
				retval = -EAGAIN;
				goto exit;
			}
		}
		// There's still need to set the timeout and timer, so this case must...
		// fall through
	case _IOC_NR(SET_RECV_TIMEOUT):
		mutex_lock(&(s_data->metadata_lock));
		printk("%s: setting timeouts", MODNAME);

		if (_IOC_NR(cmd) == _IOC_NR(SET_SEND_TIMEOUT))
			s_data->send_timeout = ktime_set(0, arg * NSEC_PER_USEC);
		else
			s_data->recv_timeout = ktime_set(0, arg * NSEC_PER_USEC);
		
		printk("%s: correct driver is number %d", MODNAME, 
				DRIVER_INDEX(s_data->send_timeout, s_data->recv_timeout));
		// Select correct driver
		filp->f_op = &f_ops[DRIVER_INDEX(s_data->send_timeout, s_data->recv_timeout)];

		mutex_unlock(&(s_data->metadata_lock));

		break;
	case _IOC_NR(REVOKE_DELAYED_MESSAGES):
		atomic_long_set((atomic_long_t *) &(s_data->ts->time), ktime_get());
		__revoke(s_data->ts, &(d->work_queue));
		
		break;
	}

	exit:
	// Release the acquired reference
	__release_s_data(s_data);

	return retval;
}

// Real flush implementation, just set the flush timestamp and wake all waiting 
// threads; all the delayed operations will flush their messages when they will 
// execute
static void __flush(struct file_data *d) {
	ktime_t old_flush; // Old flush time
	ktime_t now = ktime_get();
	struct hr_work *w;

	retry:
	printk("%s: setting flush timestamp to %lld\n", MODNAME, now);
	old_flush = d->flush_time;
	if (ktime_after(now, old_flush))
		if (atomic_long_cmpxchg((atomic_long_t *) &(d->flush_time), 
								old_flush, now) != old_flush)
			goto retry; // Someone else changed the value, retry for safety
	
	wake_up_ordered_all(&(d->wait_queue)); // Wake up all waiting readers

	// Start all work items that would have been valid at a time later than the 
	// current flush time; works are built as to auto-deallocate themselves if 
	// this happens
	printk("%s: flushing the workqueue\n", MODNAME);
	start_work_if(&(d->work_queue), w, ktime_after(w->time, d->flush_time));
}

// Revoke all messages not yet pushed to the queue and wake up all waiting 
// readers; the exposed VFS operation is just an wrapper for __flush
static int dev_flush(struct file *filp, void *id) {
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
		return MAJOR;
	}

	printk("%s: registered as %d\n", MODNAME, MAJOR);

	// Initialize the data struct for all possible file instances
	for (i = 0; i < MINORS; i++) {
		files[i] = (struct file_data) {
			.message_queue = NEW_LF_QUEUE,
			.stored_bytes = ATOMIC_LONG_INIT(0),
			.flush_time = ktime_set(0, 0) // Initial flush has time 0
		};

		init_ordered_wait_queue(files[i].wait_queue);
		init_hr_work_queue(files[i].work_queue);
	}

	return retval;
}

void cleanup_module(void) {
	int i;

	__unregister_chrdev(MAJOR, 0, MINORS, DEVICE_NAME);
	printk("%s: unregistered device\n", MODNAME);

	// Flush all files; sets all delayed work to flush their messages and wakes 
	// up any readers that are still waiting
	for (i = 0; i < MINORS; i++) {
		printk("%s: flushing device #%d\n", MODNAME, i);
		__flush(&(files[i]));
	}

	for (i = 0; i < MINORS; i++) {
		printk("%s: cleaning device #%d\n", MODNAME, i);
		// Stop all hr_work_queue daemons
		if (files[i].work_queue.daemon) {
			destroy_hr_work_queue(files[i].work_queue);
		}

		// calling __read_common with buff == NULL just flushes the message 
		// from the queue
		while (__read_common(&files[i], NULL, 1, NULL) > 0);
	}

	return;
}
