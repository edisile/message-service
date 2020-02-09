#include "../gcc_wrappers/gcc_wrappers.h"

#define NEW_RING_BITMASK ((struct ring_bitmask) {.status = {0}, .free = 0, .used = 0})
#define BITMASK_LEN 1024

#define __status(rb, i) ((rb)->status[i % BITMASK_LEN])
#define is_enabled(rb, i) (__status(rb, i))
#define is_disabled(rb, i) (!__status(rb, i))

// TODO: make this more compact using single bits for each elem
struct ring_bitmask {
	unsigned char status[BITMASK_LEN];
	long free, used; // Indices for the ring
};

static long get_free(struct ring_bitmask *rb);
static void free_unused(struct ring_bitmask *rb);
static void put_used(struct ring_bitmask *rb, long i);
static void free_unused(struct ring_bitmask *rb);
static long put_all(struct ring_bitmask *rb);

static long get_free(struct ring_bitmask *rb) {
	long i = -1;

	if (rb->free < rb->used + BITMASK_LEN) {
		// There are still free elements
		i = __atomic_add(&(rb->free), 1);
		__atomic_inc(&(rb->status[i % BITMASK_LEN]));
	}

	return i;
}

static void inline __put_used(struct ring_bitmask *rb, long i) {
	if (is_enabled(rb, i))
		__atomic_dec(&(rb->status[i % BITMASK_LEN]));
}

static void put_used(struct ring_bitmask *rb, long i) {
	__put_used(rb, i);
	
	if (i == rb->used)
		free_unused(rb);
}

static void free_unused(struct ring_bitmask *rb) {
	while ((rb->used <= rb->free) && is_disabled(rb, rb->used)) {
		__put_used(rb, rb->used);
		__atomic_add(&(rb->used), 1);
	}
}

static long put_all(struct ring_bitmask *rb) {
	long i = -1;

	while (rb->used <= rb->free) {
		put_used(rb, rb->used);
		i = __atomic_add(&(rb->used), 1);
	}

	return i;
}