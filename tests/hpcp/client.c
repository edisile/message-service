#define _GNU_SOURCE 1
#include <unistd.h>
#include <sys/mman.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <string.h>

#include "common.h"

int mfd, fd;
pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
unsigned long flen = 0;

char terminate = 0;

#define C_THREADS 5
pthread_t c_thread_data[C_THREADS];

void *client_thread_job(void *data) {
	struct chunk c;
	// printf("Reader thread started\n");

	while (!terminate) {
		int r = read(mfd, &c, sizeof(struct chunk));

		if (r <= 0) continue;

		// printf("Read %dB from device\n", r);

		if (c.len < CHUNK_SIZE) {
			__sync_fetch_and_add(&terminate, (char) 1);
		}

		unsigned long off = c.index * CHUNK_SIZE;

		// printf("Reader copying to dest with offset %lu, current size %lu\n", off, map_size);
		pwrite(fd, &c.bytes, c.len, off);
		__sync_fetch_and_add(&flen, c.len);
		// printf("Reader copied\n");
	}
}

int client(char *dest) {
	char path[1024];
	realpath(dest, (char *) &path);
	char *path1 = strdup(path);
	char *dir = dirname(path);
	char *file = basename(path1);
	free(path1);

	if (chdir(path) != 0) {
		__exit("Chdir failed");
	}

	fd = open(file, O_RDWR | O_CREAT, 0664);
	if (fd == -1) {
		__exit("File open failed");
	}

	mfd = open(MSG_DEV, O_RDONLY | O_CREAT);
	if (mfd == -1) {
		__exit("Open failed");
	}

	// ioctl(mfd, 10000, 10000); // Set recieve timeout at 0.01 seconds

	for (int i = 0; i < C_THREADS; i++) {
		// printf("Launch reader #%d\n", i);
		pthread_create(&c_thread_data[i], NULL, client_thread_job, NULL);
	}

	for (int i = 0; i < C_THREADS; i++) {
		pthread_join(c_thread_data[i], NULL);
	}

	ftruncate(fd, flen);
	// printf("Truncated file to %luB\n", flen);

	return 0;
}