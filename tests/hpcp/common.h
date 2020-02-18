#include <error.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#define CHUNK_SIZE (1 << 12) // 4 KiB chunks

struct chunk {
	unsigned int index;
	unsigned short len;
	unsigned char bytes[CHUNK_SIZE];
};

void __exit(char *error);

#ifndef MSG_DEV
	#define MSG_DEV "/dev/messages"
#endif