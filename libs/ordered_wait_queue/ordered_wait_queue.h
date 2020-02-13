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

struct ordered_wait_queue_entry {
	struct list_head list;
	struct task_struct *tcb;
	ktime_t ts;
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
	rcu_read_unlock();

	mutex_lock(&(owq->lock));
	// Add e to the list in front of x (list_add_tail_rcu, unlike the name might 
	// suggest, does exactly this: assigns x->list.prev = &e and viceversa)
	list_add_tail_rcu(&(e->list), &(x->list));
	mutex_unlock(&(owq->lock));

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
static inline void ordered_queue_detach(struct ordered_wait_queue *owq, 
										struct ordered_wait_queue_entry *e) {
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
																					\
	rcu_read_lock();																\
	last = list_entry_rcu((owq)->list.prev, struct ordered_wait_queue_entry, list);	\
	last_ts = last->ts;																\
	rcu_read_unlock();																\
																					\
	if (ktime_after(e.ts, last_ts))													\
		ordered_queue_append(owq, &e);												\
	else																			\
		ordered_queue_insert(owq, &e);												\
																					\
	printk("	thread %d went to sleep", current->pid);					\
	wait_ret = wait_event_interruptible_hrtimeout((owq)->wq, condition, timeout);	\
	printk("	thread %d woke up, reason %d", current->pid, wait_ret);				\
	ordered_queue_detach(owq, &e);													\
																					\
	wait_ret;																		\
})

// Picks the first task from the queue (if not empty) and wakes it up
static void inline wake_up_ordered(struct ordered_wait_queue *owq) {
	struct ordered_wait_queue_entry *f;
	f = list_first_or_null_rcu(&(owq->list), struct ordered_wait_queue_entry, list);
	if (f != NULL) 
		wake_up_process(f->tcb);
}

#define wake_up_ordered_all(owq) (wake_up_all(&((owq)->wq)))