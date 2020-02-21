#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>

#include "common.h"

unsigned char *map;
unsigned long len;
int mfd;
unsigned long chunk_ind = 0;

#ifndef S_THREADS
	#define S_THREADS 5
#endif
pthread_t s_thread_data[S_THREADS];

#define min(x, y) (x < y ? x : y)

void *server_thread_job(void *data) {
	struct chunk c;
	int ret;
	int i = __sync_fetch_and_add(&chunk_ind, 1);

	for ( ; i * CHUNK_SIZE < len; i = __sync_fetch_and_add(&chunk_ind, 1)) {
		c.index = i;
		c.len = min(CHUNK_SIZE, len - i * CHUNK_SIZE);
		memcpy(&c.bytes, map + i * CHUNK_SIZE, c.len);

		retry:
		ret = write(mfd, &c, sizeof(struct chunk));
		if (ret < 0) {
			if (errno != ENOSPC) {
				// fprintf(stderr, "Error %d", errno);
				__exit("Write fucked up");
			} else {
				usleep(100000);
				goto retry;
			}
		} 
		// printf("Wrote %dB to device\n", ret);
	}

	return NULL;
}

int server(char *orig) {
	int fd = open(orig, O_RDONLY);
	if (fd == -1) {
		__exit("Open failed");
	}

	// Get file length
	len = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	// mmap the file
	map = (unsigned char *) mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		__exit("mmap failed");
	}

	mfd = open(MSG_DEV, O_WRONLY | O_CREAT);
	if (mfd == -1) {
		__exit("Open failed");
	}

	for (int i = 0; i < S_THREADS; i++) {
		pthread_create(&s_thread_data[i], NULL, server_thread_job, NULL);
	}

	for (int i = 0; i < S_THREADS; i++) {
		pthread_join(s_thread_data[i], NULL);
	}

	return 0;
}