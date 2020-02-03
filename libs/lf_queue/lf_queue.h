#include <stddef.h>

// Conveniently named wrappers for GCC built-ins
#define __atomic_inc(a) (__sync_fetch_and_add(a, 1))
#define __atomic_dec(a) (__sync_fetch_and_sub(a, 1))
#define __atomic_swap(ptr, old, new) (__sync_bool_compare_and_swap(ptr, old, new))
#define NEW_LF_QUEUE ((struct lf_queue) {.head = NULL, .tail = NULL})
#define DEFINE_LF_QUEUE(name) struct lf_queue name = NEW_LF_QUEUE

struct lf_queue_node {
	struct lf_queue_node *next;
	int counter;
};

struct lf_queue {
	struct lf_queue_node *head, *tail;
};


void __cleanup(struct lf_queue_node *elem) {
	// Make sure elem is clean
	elem->counter = 0;
	elem->next = NULL;
}


void lf_queue_push(struct lf_queue *q, struct lf_queue_node *elem) {
	int ok = 0;
	struct lf_queue_node *prev = NULL;

	if (elem == NULL) {
		// What are you even doing here?
		//printf("lf_queue_push: called with NULL lf_queue_node pointer");
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


struct lf_queue_node *lf_queue_pull(struct lf_queue *q) {
	int ok = 0;
	struct lf_queue_node *elem;

	// Try to hide the head of the queue from anyone else
	retry:
	if (q->head == NULL) return NULL; // Empty queue
	
	elem = q->head;
	__atomic_inc(&(elem->counter));
	ok = __atomic_swap(&(q->head), elem, elem->next);
	if (!ok) {
		// Someone pulled this node already, retry
		__atomic_dec(&(elem->counter));
		goto retry;
	}
	
	while (elem->counter > 1) {
		// usleep(250); // Give others time to leave this node alone
		// TODO: maybe replace with some wait event queue?
	}

	// If someone appends something to this elem while dequeueing the last elem
	// the list is marked as empty and this is an error!
	// The state of the list in this situation:
	// 		tail is neither NULL nor elem (correct)
	// 		head is NULL (wrong)
	// Is elem the tail? Then list is empty
	__atomic_swap(&(q->tail), elem, NULL);
	// Is head NULL? Maybe check if someone pushed in the meanwhile
	__atomic_swap(&(q->head), NULL, elem->next);

	// Clean elem up to separate it from the rest of the list
	__cleanup(elem);

	return elem;
}
