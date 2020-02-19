#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/rculist.h>
#include <linux/sched.h>
#include <linux/wait.h>

struct ordered_wait_queue {
	struct list_head list;
	struct wait_queue_head wq;
	struct mutex lock;
	atomic_t population;
};

#define __SLEEPING 0
#define __WOKEN_UP 1

struct ordered_wait_queue_entry {
	struct list_head list;
	struct task_struct *tcb;
	ktime_t ts;
	atomic_t status;
};

#define init_ordered_wait_queue(owq) ({											\
	init_waitqueue_head(&(owq.wq));												\
	INIT_LIST_HEAD(&(owq.list));												\
	mutex_init(&(owq.lock));													\
	atomic_set(&(owq.population), 0);											\
})

static inline void ordered_queue_insert(struct ordered_wait_queue *owq, 
										struct ordered_wait_queue_entry *e) {
	struct ordered_wait_queue_entry *x;

	rcu_read_lock();
	list_for_each_entry_rcu(x, &(owq->list), list) {
		// Traverse list looking for the first node with a timestamp larger than 
		// e->ts; when found insert e->list before x->list
		if (ktime_before(e->ts, x->ts))
			break;
	}

	mutex_lock(&(owq->lock));
	// Add e to the list in front of x (list_add_tail_rcu, unlike the name might 
	// suggest, does exactly this: assigns x->list.prev = &e and viceversa)
	list_add_tail_rcu(&(e->list), &(x->list));
	mutex_unlock(&(owq->lock));
	
	rcu_read_unlock();

	atomic_inc(&(owq->population));
}

static inline void ordered_queue_append(struct ordered_wait_queue *owq, 
										struct ordered_wait_queue_entry *e) {
	mutex_lock(&(owq->lock));
	list_add_tail_rcu(&(e->list), &(owq->list));
	mutex_unlock(&(owq->lock));

	atomic_inc(&(owq->population));
}

// Task removes its own ordered_wait_queue_entry from the queue
static inline void __ordered_queue_detach(struct ordered_wait_queue *owq, 
										struct ordered_wait_queue_entry *e) {
	struct ordered_wait_queue_entry *f;

	// Make sure current thread is the head of the list
	for ( ; ; ) {
		// printk("	thread %d trying to detach", current->pid);
		
		rcu_read_lock();
		f = list_first_or_null_rcu(&(owq->list), 
									struct ordered_wait_queue_entry, list);
		rcu_read_unlock();

		if (e == f) {
			// printk("	thread %d detach success", current->pid);
			break;
		} else {
			// printk("	thread %d detach sleep", current->pid);
			usleep_range(10, 50); // Don't busy loop
		}
	}
	
	// The real detach
	mutex_lock(&(owq->lock));
	list_del_rcu(&(e->list));
	mutex_unlock(&(owq->lock));
	atomic_dec(&(owq->population));
}

#define wait_event_ordered_interruptible_hrtimeout(owq, condition, t, timeout) ({	\
	struct ordered_wait_queue_entry e, *last;										\
	ktime_t last_ts;																\
	int wait_ret;																	\
	e.ts = t;																		\
	e.tcb = current;																\
	atomic_set(&e.status, __SLEEPING);											\
																					\
	rcu_read_lock();																\
	last = list_entry_rcu((owq)->list.prev, struct ordered_wait_queue_entry, list);	\
	last_ts = last->ts;																\
																					\
	if (ktime_after(e.ts, last_ts))													\
		ordered_queue_append(owq, &e);												\
	else																			\
		ordered_queue_insert(owq, &e);												\
																					\
	rcu_read_unlock();																\
																					\
	/*printk("	thread %d went to sleep", current->pid);*/							\
	wait_ret = wait_event_interruptible_hrtimeout((owq)->wq, condition, timeout);	\
	__ordered_queue_detach(owq, &e);												\
	printk("	thread %d woke up at %lld, reason %d, entered at %lld", 			\
			current->pid, ktime_get(), wait_ret, e.ts);								\
																					\
	wait_ret;																		\
})

// Picks the first task from the queue (if not empty) and wakes it up
static void inline wake_up_ordered(struct ordered_wait_queue *owq) {
	struct ordered_wait_queue_entry *x;

	rcu_read_lock();
	list_for_each_entry_rcu(x, &(owq->list), list) {
		// Traverse list looking for the first node not yet woken up
		if (atomic_cmpxchg(&(x->status), __SLEEPING, __WOKEN_UP) == __SLEEPING) {
			wake_up_process(x->tcb);
			break;
		}
		// If atomic_cmpxchg failed someone else already woke up that thread, 
		// try to wake up the next one
	}
	rcu_read_unlock();
}

#define wake_up_ordered_all(owq) (wake_up_all(&((owq)->wq)))