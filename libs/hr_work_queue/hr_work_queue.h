#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/rculist.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/workqueue.h>


struct hr_work_queue {
	struct hrtimer_sleeper timer;
	struct wait_queue_head wait_q;
	struct task_struct *daemon;
	struct list_head list;
	struct mutex lock;
	ktime_t next_wakeup;

	//struct workqueue_struct *work_q; // no, use system_unbound_wq instead
};

// When hrtimer_sleeper callback is called the sleeper thread is woken up and the 
// task field in the struct is set to NULL
#define TIMER_FIRED(hrt_sleeper) (hrt_sleeper.task == NULL)

struct hr_work {
	struct work_struct work;
	struct list_head list;
	ktime_t time; // Time when the work should be executed
};


// Needed to set up the sleeper
#define __reset_sleeper(hr_sleeper) (hrtimer_init_sleeper(hr_sleeper,	\
										CLOCK_MONOTONIC, HRTIMER_MODE_ABS))

// Init work queue, wait_queue, timer and lock
#define init_hr_work_queue(wq) ({										\
	INIT_LIST_HEAD(&(wq.list));											\
	mutex_init(&(wq.lock));												\
	init_waitqueue_head(&(wq.wait_q));									\
	wq.daemon = NULL;													\
	wq.next_wakeup = KTIME_MAX;											\
	__reset_sleeper(&(wq.timer));										\
	wq.timer.task = NULL; 												\
})

// Init a hr_work item
#define INIT_HR_WORK(_work, _func, _time) ({							\
	INIT_WORK(&(_work.work), _func);									\
	_work.time = _time;													\
})

// Init a hr_work item to start at a delay relative to current time
#define INIT_HR_WORK_REL(_work, _func, _delay) ({						\
	INIT_WORK(&(_work.work), _func);									\
	_work.time = ktime_add(_delay, ktime_get());						\
})

#define hr_work_queue_active(wq) ((wq)->daemon != NULL)

// Start all hr_work for which condition is true
#define start_work_if(hrwq, work_ptr, condition) ({						\
	bool ok;															\
	rcu_read_lock();													\
	list_for_each_entry_rcu(work_ptr, &((hrwq)->list), list) {			\
		if (condition) {												\
			mutex_lock(&((hrwq)->lock));								\
			list_del_rcu(&(work_ptr->list));							\
			mutex_unlock(&((hrwq)->lock));								\
			printk("hr_work_queue: queuing work with time %lld at %lld",\
					work_ptr->time, ktime_get());						\
			ok = queue_work(system_unbound_wq, &(work_ptr->work));		\
			if (!ok)													\
				/* Fallback, execute work locally */					\
				work_ptr->work.func(&(work_ptr->work));					\
		}																\
	}																	\
	rcu_read_unlock();													\
})

// Start hr_work if condition is true, stop as soon as it becomes false
#define start_work_if_stop_early(hrwq, work_ptr, condition) ({			\
	bool ok;															\
	rcu_read_lock();													\
	list_for_each_entry_rcu(work_ptr, &((hrwq)->list), list) {			\
		if (condition) {												\
			mutex_lock(&((hrwq)->lock));								\
			list_del_rcu(&(work_ptr->list));							\
			mutex_unlock(&((hrwq)->lock));								\
			printk("hr_work_queue: queuing work with time %lld at %lld",\
					work_ptr->time, ktime_get());						\
			ok = queue_work(system_unbound_wq, &(work_ptr->work));		\
			if (!ok)													\
				/* Fallback, execute work locally */					\
				work_ptr->work.func(&(work_ptr->work));					\
		} else {														\
			break;														\
		}																\
	}																	\
	rcu_read_unlock();													\
})

// Execute all hr_work on the thread making the call
#define execute_all_work(hrwq, work_ptr) ({								\
	rcu_read_lock();													\
	list_for_each_entry_rcu(work_ptr, &((hrwq)->list), list) {			\
		mutex_lock(&((hrwq)->lock));									\
		list_del_rcu(&(work_ptr->list));								\
		mutex_unlock(&((hrwq)->lock));									\
		printk("hr_work_queue: executing work with time %lld at %lld",	\
				work_ptr->time, ktime_get());							\
		/* Execute work locally */										\
		work_ptr->work.func(&(work_ptr->work));							\
	}																	\
	rcu_read_unlock();													\
})

// Stop the hr_work_queue: cancel the timer, kill the daemon then start all 
// residual works in the queue
#define destroy_hr_work_queue(wq) ({ \
	struct hr_work *w;													\
	hrtimer_cancel(&(wq.timer.timer));									\
	kthread_stop(wq.daemon);											\
	execute_all_work(&wq, w); /* Clear the rest of the work */			\
})

// Daemon work
// Wait on the wait queue for the timer to fire or for kthread_should_stop()
// Call the above macro with the condition hr_work->time <>> ktime_get(); when 
// hr_work->time > ktime_get() set the timer to fire at that time or if the list 
// is empty disable the timer.
static int daemon_work(void *hr_work_q) {
	struct hr_work_queue *hrwq = (struct hr_work_queue *) hr_work_q;
	struct hr_work *w;

	hrwq->daemon = current; // This marks the queue as active

	printk("hr_work_queue: daemon thread %d started", current->pid);
	__reset_sleeper(&(hrwq->timer));

	for ( ; ; ) {
		printk("hr_work_queue: daemon thread %d going to sleep", current->pid);
		wait_event(hrwq->wait_q, kthread_should_stop() || 
					TIMER_FIRED(hrwq->timer));
		printk("hr_work_queue: daemon thread %d woke up", current->pid);

		if (kthread_should_stop()) {
			printk("hr_work_queue: daemon thread %d should stop", current->pid);
			mutex_lock(&(hrwq->lock));
			hrwq->daemon = NULL;
			mutex_unlock(&(hrwq->lock));

			return 0;
		}
		
		// Re-init the sleeper to sleep again at the next iteration
		__reset_sleeper(&(hrwq->timer));
		
		// Iterate on list and enqueue all works whose timestamp is before 
		// current time
		start_work_if_stop_early(hrwq, w, ktime_before(w->time, ktime_get()));

		// Try to restart the timer by checking if there's other hq_work queued
		rcu_read_lock();
		w = list_first_or_null_rcu(&(hrwq->list), struct hr_work, list);
		if (w != NULL) {
			atomic_long_set((atomic_long_t *) &(hrwq->next_wakeup), 
							(long) w->time);
			hrtimer_start(&(hrwq->timer.timer), w->time, HRTIMER_MODE_ABS);
		} else
			atomic_long_set((atomic_long_t *) &(hrwq->next_wakeup), 
							(long) KTIME_MAX);
		rcu_read_unlock();
	}
}

// Push to queue
// Iterate on the list and insert the new hr_work; if this work is the first in 
// queue cancel the timer and set it to the new time
static bool queue_hr_work(struct hr_work_queue *hrwq, struct hr_work *work) {
	struct hr_work *w, *last;
	struct list_head *head;

	if (!hr_work_queue_active(hrwq)) {
		printk("hr_work_queue: can't add to an inactive queue!");
		return (bool) 0;
	}

	rcu_read_lock();

	last = list_entry_rcu(hrwq->list.prev, struct hr_work, list);

	if (ktime_after(work->time, last->time)) {
		// Fast path, will just need to append without iterating
		head = &(hrwq->list);
	} else {
		// Iterate in the list looking for the first work that comes after 
		// current one
		list_for_each_entry_rcu(w, &(hrwq->list), list) {
			if (ktime_before(work->time, w->time)) {
				break;
			}
		}
		head = &(w->list);
	}

	mutex_lock(&(hrwq->lock));
	list_add_tail_rcu(&(work->list), head); // Add work before head
	mutex_unlock(&(hrwq->lock));
	
	rcu_read_unlock();

	// Should driver be restarted?
	if (ktime_before(work->time, hrwq->next_wakeup)) {
		printk("hr_work_queue: gotta reset timer after queueing work");
		// Current work is more urgent, dearm and rearm the timer
		if (hrtimer_try_to_cancel(&(hrwq->timer.timer)) >= 0) {
			printk("hr_work_queue: timer cancelled");
			atomic_long_set((atomic_long_t *) &(hrwq->next_wakeup), 
							(long) work->time);
			
			// while (!hr_work_queue_active(hrwq)) usleep_range(10, 50);
			
			hrtimer_start(&(hrwq->timer.timer), work->time, HRTIMER_MODE_ABS);
			printk("hr_work_queue: timer restarted");
		}
	}

	return (bool) 1;
}

#define start_hr_work_queue(wq) ({										\
	struct task_struct *tsk;											\
	printk("hr_work_queue: taking lock");								\
	mutex_lock(&((wq)->lock));											\
	if (!hr_work_queue_active(wq)) {									\
		printk("hr_work_queue: starting");								\
		tsk = kthread_run(daemon_work, wq, "hr_q_worker");				\
		if (tsk == -ENOMEM)	printk("hr_work_queue: can't start");		\
	}																	\
	mutex_unlock(&((wq)->lock));										\
	tsk;																\
})
