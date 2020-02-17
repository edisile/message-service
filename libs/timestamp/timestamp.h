#include <linux/kernel.h>
#include <linux/ktime.h>

// A timestamp with a reference counter
struct timestamp {
	ktime_t time;
	atomic_t refs;
};

#define __acquire_timestamp(ts) ({atomic_inc(&(ts->refs)); ts;})
#define __release_timestamp(ts) if (atomic_dec_and_test(&ts->refs)) vfree(ts)