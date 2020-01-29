#include <stddef.h>
#include <unistd.h>

// Conveniently named wrappers for GCC built-ins
#define atomic_inc(a) (__sync_fetch_and_add(a, 1))
#define atomic_dec(a) (__sync_fetch_and_sub(a, 1))
#define atomic_swap(ptr, old, new) (__sync_bool_compare_and_swap(ptr, old, new))

struct lf_queue_node {
	struct lf_queue_node *next;
	int counter;
};

struct lf_queue {
	struct lf_queue_node *head, *tail;
};



void enqueue(struct lf_queue *q, struct lf_queue_node *elem) {
	char ok = 0;
	struct lf_queue_node *prev;

	elem->counter = 0; // Make sure elem is clean
	elem->next = NULL;

	retry:
	// Try to reserve the last place in the list
	if (q->tail != NULL)
		atomic_inc(&(q->tail->counter));
	
	prev = q->tail;
	ok = atomic_swap(&(q->tail), prev, elem);
	if (!ok) {
		// Someone else took the last place, retry
		if (prev != NULL)
			atomic_dec(&(prev->counter));
		goto retry;
	}

	// Now elem is the tail of the list, if anyone else enqueues anything it's 
	// gonna be behind this elem; time to actually attach elem to the list
	if (prev != NULL) {
		// Old tail was a real node, attach elem to it
		prev->next = elem;
		atomic_dec(&(prev->counter));
	} else {
		// Old tail was NULL, the queue was empty
		q->head = elem; // TODO: maybe an atomic store is better?
	}
}


struct lf_queue_node *dequeue(struct lf_queue *q) {
	char ok = 0;
	struct lf_queue_node *elem;

	retry:
	// Try to hide the head of the queue from anyone else
	if (q->head == NULL) return NULL; // Empty queue
	
	atomic_inc(&(q->head->counter));
	elem = q->head;
	ok = atomic_swap(&(q->head), elem, elem->next);
	if (!ok) {
		// Someone pulled this node already, retry
		atomic_dec(&(elem->counter));
		goto retry;
	}
	
	int retries = 0;
	while (elem->counter > 1) {
		usleep(100); // Give others time to leave this node alone
		if (++retries > 5) {
			atomic_dec(&(elem->counter));
			goto retry;
		};
	}

	// If someone appends something to this elem while dequeueing the last elem
	// the list is marked as empty and this is an error:
	// 		tail is neither NULL nor elem (correct)
	// 		head is NULL (wrong)
	atomic_swap(&(q->tail), elem, NULL); // elem is the tail? list is empty
	atomic_swap(&(q->head), NULL, elem->next);

	// Clean elem up and separate from the rest of the list
	elem->next = NULL;
	elem->counter = 0;

	return elem;
}