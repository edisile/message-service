#include "../gcc_wrappers/gcc_wrappers.h"

#define NEW_RING_BITMASK ((struct ring_bitmask) {.status = {0}, .free = 0, .used = 0})
#define BITMASK_LEN 1024

// TODO: make this more compact using single bits for each elem
struct ring_bitmask {
	unsigned char status[BITMASK_LEN];
	long free, used; // Indices for the ring
};

long get_free(struct ring_bitmask *rb) {
    long i = -1;

    if (rb->free < rb->used + BITMASK_LEN) {
        // There are still free elements
        i = __atomic_add(&(rb->free), 1);
        __atomic_inc(&(rb->status[i % BITMASK_LEN]));
    }

    return i;
}

long put_used(struct ring_bitmask *rb) {
    long i = -1;

    if (rb->used != rb->free) {
        // There are still used elements
        i = __atomic_add(&(rb->used), 1);
        __atomic_dec(&(rb->status[i % BITMASK_LEN]));
    }

    return i;
}

#define get_value(rb, i) (rb->status[i % BITMASK_LEN])