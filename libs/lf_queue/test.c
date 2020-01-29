#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "lf_queue.h"

#define container_of(ptr, type, member) ({            \
 const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
 (type *)( (char *)__mptr - offsetof(type,member) );})

struct data {
	int d;
	struct lf_queue_node node;
};

void *thread_job(void * arg) {
	struct lf_queue *queue = (struct lf_queue*) arg;
	struct lf_queue_node *n;
	struct data *x;

	int i = 0;

	while (i < 50) {
		float r = (float) random() / RAND_MAX;

		if (r > 0.5) {
			//printf("enqueue\n");
			x = (struct data*) malloc(sizeof(struct data));
			x->d = i;
			i++;
			enqueue(queue, &(x->node));
		} else {
			//printf("dequeue\n");
			n = dequeue(queue);
			if (n == NULL) continue;

			x = container_of(n, struct data, node);
			printf("%d ", x->d);
			free(x);
		}
		
		usleep(random() % 1000);
	}

	return NULL;
}

int main() {
	struct lf_queue queue;
	struct lf_queue_node *node_ptr;

	queue.head = queue.tail = NULL;

	pthread_t threads[10];

	for (int i = 0 ; i < 10; i++) {
		if (pthread_create(&threads[i], NULL, thread_job, (void *) &queue))
			return EXIT_FAILURE;
	}

	for (int i = 0 ; i < 10; i++) {
		pthread_join(threads[i], NULL);
		printf("\nended_%d\n", i);
	}

	while ((node_ptr = dequeue(&queue)) != NULL) {
		struct data *x = container_of(node_ptr, struct data, node);
		printf("%d ", x->d);
	}

	return EXIT_SUCCESS;
}