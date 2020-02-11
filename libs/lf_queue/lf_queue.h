#include <stddef.h>
#include "../gcc_wrappers/gcc_wrappers.h"
#ifdef __KERNEL__
	#include <linux/delay.h>
	#define __sleep_range(min, max) (usleep_range(min, max))
#else
	#include <unistd.h>
	__sleep_range(min, max) (usleep((min + max) / 2))
#endif

#define NEW_LF_QUEUE ((struct lf_queue) {.head = NULL, .tail = NULL})
#define DEFINE_LF_QUEUE(name) struct lf_queue name = NEW_LF_QUEUE
#define IS_EMPTY(q) (__atomic_sub(&q.head, 0) == NULL)
#define __SLEEP_MIN 10
#define __SLEEP_MAX 50

struct lf_queue_node {
	struct lf_queue_node *next;
	int counter;
};

struct lf_queue {
	struct lf_queue_node *head, *tail;
};


static void __cleanup(struct lf_queue_node *elem) {
	// Make sure elem is clean
	elem->counter = 0;
	elem->next = NULL;
}


static void lf_queue_push(struct lf_queue *q, struct lf_queue_node *elem) {
	int ok = 0;
	struct lf_queue_node *prev = NULL;

	if (elem == NULL) {
		// What are you even doing here?
		#ifdef __KERNEL__
			printk("lf_queue_push: called with NULL lf_queue_node pointer!");
		#else
			printf("lf_queue_push: called with NULL lf_queue_node pointer!");
		#endif
		
		return;
	}

	__cleanup(elem);

	// Try to reserve the last place in the list
	retry:
	if (q->tail != NULL) {
		prev = q->tail;
		__atomic_inc(&(prev->counter));
	}
	
	ok = __atomic_swap(&(q->tail), prev, elem);
	if (!ok) {
		// Someone else took the last place, retry
		if (prev != NULL)
			__atomic_dec(&(prev->counter));
		goto retry;
	}

	// Now elem is the tail of the list, if anyone else enqueues anything it's 
	// gonna be behind this elem; time to actually attach elem to the list
	if (prev != NULL) {
		// Old tail was a real node, attach elem to it
		prev->next = elem;
		__atomic_dec(&(prev->counter));
	} else {
		// Old tail was NULL, the queue was empty
		q->head = elem;
		__sync_synchronize(); // Memory barrier, update is visible immediately
	}
}


static struct lf_queue_node *lf_queue_pull(struct lf_queue *q) {
	int ok, head_is_tail;
	struct lf_queue_node *elem;

	// Try to "hide" the head of the queue from anyone else
	retry:
	if (q->head == NULL) return NULL; // Empty queue
	
	head_is_tail = (q->head == q->tail);
	elem = q->head;
	__atomic_inc(&(elem->counter));
	ok = __atomic_swap(&(q->head), elem, elem->next);
	if (!ok) {
		// Someone pulled this node already, retry
		__atomic_dec(&(elem->counter));
		goto retry;
	}
	
	while (elem->counter > 1) {
		__sleep_range(__SLEEP_MIN, __SLEEP_MAX);
		// Give others time to leave this node alone
	}

	// If someone appends something to this elem while dequeueing the last elem
	// the list is marked as empty and this is an error!
	// The state of the list in this situation:
	// 		tail is neither NULL nor elem (correct)
	// 		head is NULL (wrong)
	if (head_is_tail) {
		// Is elem the tail? Then list is empty
		__atomic_swap(&(q->tail), elem, NULL);
		// Is head NULL? Check if someone pushed in the meantime
		__atomic_swap(&(q->head), NULL, elem->next);
	}

	// Clean elem up to separate it from the rest of the list
	__cleanup(elem);

	return elem;
}
