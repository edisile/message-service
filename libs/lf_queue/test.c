#include <stdio.h>
#include <stdlib.h>

#include "lf_queue.h"

void test() {
	struct lf_queue queue;
	struct lf_queue_node node, *node_ptr;

	queue.head = queue.tail = NULL;

	printf("%p\n", (void *) &node);
	enqueue(&queue, &node);

	printf("%p %p\n", (void *) queue.head, (void *) queue.tail);
	node_ptr = dequeue(&queue);

	printf("%p\n", (void *) node_ptr);
	printf("%p %p\n", (void *) queue.head, (void *) queue.tail);
}

int _start() {
	test();
	exit(0);
}