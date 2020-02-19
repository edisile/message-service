#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "server.h"
#include "client.h"

void __exit(char *error) {
	perror(error);
	_exit(errno);
}

int main(int argc, char const *argv[]) {
	char *orig, *dest;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s ORIGIN DESTINATION\n", argv[0]);
		return 1;
	}

	orig = strdup(argv[1]);
	dest = strdup(argv[2]);
	// Fork: server read the file and writes it to MESS_DEV, client reads from 
	// MESS_DEV and writes it to the destination

	pid_t pid = fork();
	
	switch (pid) {
		case -1:
			__exit("Fork failed");
		case 0:
			return client(dest);
		default:
			return server(orig);
	}
}
