#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/syscall.h>

int mfd;

#ifndef THREADS
	#define THREADS 10
#endif

#ifndef SEND_DELAY
	#define SEND_DELAY 500000
#endif

#ifndef RECV_DELAY
	#define RECV_DELAY 1000000
#endif

void *thread_job(void *data) {
	char message[256] = { 0 };

	pid_t x = syscall(__NR_gettid);

	// printf("Thread #%d: reading\n", x);
	int r = read(mfd, (char *) message, 255);
	
	if (r >= 0) {
		printf("Thread #%d: received '%s'\n", x, message);
		// fflush(stdout);
	} else {
		perror("Woke up with error");
	}

	return NULL;
}

int main(int argc, char const *argv[]) {
	pthread_t thread_data[THREADS];
	char message[256];

	if (argc != 2) {
		fprintf(stderr, "Usage: %s MESS_DEV\n", argv[0]);
		return 1;
	}

	mfd = open(argv[1], O_RDWR);
	if (mfd == -1) {
		perror("Open failed");
		return 1;
	}

	if (SEND_DELAY > 0)
		ioctl(mfd, 9999, SEND_DELAY); // Set send delay
	if (RECV_DELAY > 0)
		ioctl(mfd, 10000, RECV_DELAY); // Set receive timeout

	for (int i = 0; i < THREADS; i++) {
		// printf("Launch reader #%d\n", i);
		pthread_create(&thread_data[i], NULL, thread_job, NULL);
		// usleep(100);
	}

	// Unblock the waiting readers
	for (int i = 0; i < THREADS; i++) {
		// printf("Writing\n");
		sprintf(message, "%d", i);
		int w = write(mfd, message, strlen(message));

		if (w <= 0) {
			perror("Write failed");
		}
	}

	#ifdef REVOKE
	ioctl(mfd, 10001); // Revoke messages
	#endif

	#ifndef DONT_WAIT
	for (int i = 0; i < THREADS; i++) {
		pthread_join(thread_data[i], NULL);
	}
	#endif

	#ifdef DONT_WAIT
	usleep(300000);
	#endif

	return close(mfd);
}
