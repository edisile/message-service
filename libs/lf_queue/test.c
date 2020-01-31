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

struct lf_queue queue;
int extracted;

void *thread_job(void * arg) {
	struct lf_queue *queue = (struct lf_queue*) arg;
	struct lf_queue_node *n;
	struct data *x;

	int i = 0;

	while (i < 50) {
		float r = (float) random() / RAND_MAX;

		if (r > 0.5) {
			//printf("push\n");
			x = (struct data*) malloc(sizeof(struct data));
			x->d = i;
			i++;
			push(queue, &(x->node));
		} else {
			//printf("pull\n");
			n = pull(queue);
			if (n == NULL) continue;

			x = container_of(n, struct data, node);
			printf("%2d ", x->d);
			free(x);
		}
		
		usleep(random() % 500);
	}

	return NULL;
}

void *thread_job2(void * arg) {
	struct lf_queue *queue = (struct lf_queue*) arg;
	struct data *x;

	for (int i = 0; i < 50; i++) {
		usleep(random() % 100);
		x = (struct data*) malloc(sizeof(struct data));
		x->d = i;
		push(queue, &(x->node));
	}

	return NULL;
}

void *thread_job3(void * arg) {
	struct lf_queue *queue = (struct lf_queue*) arg;
	struct lf_queue_node *n;
	struct data *x;

	for (int i = 0; i < 50; i++) {
		usleep(random() % 100);
		n = pull(queue);
		if (n == NULL) continue;

		atomic_inc(&extracted);
		x = container_of(n, struct data, node);
		printf("%2d ", x->d);
		free(x);
	}

	return NULL;
}

int test1() {
	pthread_t threads[10];

	// Inserts
	for (int i = 0 ; i < 10; i++) {
		if (pthread_create(&threads[i], NULL, thread_job2, (void *) &queue))
			return EXIT_FAILURE;
	}

	for (int i = 0 ; i < 10; i++) {
		pthread_join(threads[i], NULL);
		//printf("\nended_%d\n", i);
	}

	int i = 0;
	for (struct lf_queue_node *n = queue.head; n != NULL; n = n->next) i++;

	printf("%d nodes found\n", i);

	// Removals
	for (int i = 0 ; i < 10; i++) {
		if (pthread_create(&threads[i], NULL, thread_job3, (void *) &queue))
			return EXIT_FAILURE;
	}

	for (int i = 0 ; i < 10; i++) {
		pthread_join(threads[i], NULL);
		//printf("\nended_%d\n", i);
	}


	printf("\nprinting remaining\n");
	struct lf_queue_node *node_ptr;
	while ((node_ptr = pull(&queue)) != NULL) {
		atomic_inc(&extracted);
		struct data *x = container_of(node_ptr, struct data, node);
		printf("%2d ", x->d);
		free(x);
	}

	printf("\nExtracted %d\n", extracted);
	return EXIT_SUCCESS;
}

int test2() {
	pthread_t threads[10];

	// Inserts
	for (int i = 0 ; i < 10; i++) {
		if (pthread_create(&threads[i], NULL, thread_job, (void *) &queue))
			return EXIT_FAILURE;
	}

	for (int i = 0 ; i < 10; i++)
		pthread_join(threads[i], NULL);

	// printf("\nprinting remaining\n");
	struct lf_queue_node *node_ptr;
	while ((node_ptr = pull(&queue)) != NULL) {
		struct data *x = container_of(node_ptr, struct data, node);
		printf("%2d ", x->d);
		free(x);
	}

	printf("\n");
	return EXIT_SUCCESS;
}

int main() {
	queue.head = queue.tail = NULL;

	return test2();
}