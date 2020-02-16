#include <asm-generic/atomic-long.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/moduleparam.h>

static int param_atomic_long_set(const char *val, const struct kernel_param *kp);

static const struct kernel_param_ops atomic_long_param_ops = {
	.set = param_atomic_long_set,
	.get = param_get_long,
};

static int param_atomic_long_set(const char *val, const struct kernel_param *kp) {
	long v; // Value for the parameter
	char c; // Measurement unit, if provided
	int ret;

	ret = sscanf(val, "%ld%c", &v, &c);

	if (ret == 0 || v < 0) {
		printk("Invalid parameter value");
		return -EINVAL;
	}

	// A measurement unit might have been passed
	switch (c) {
	case 'G':
	case 'g':
		v = v << 10;
		// fall through
	case 'M':
	case 'm':
		v = v << 10;
		// fall through
	case 'K':
	case 'k':
		v = v << 10;
		break;
	case '\n':
	case '\0':
		// It was just a number with no unit
		break;
	default:
		printk("Invalid parameter measurement unit; valid ones are kmgKMG");
		return -EINVAL;
	}

	printk("Atomically setting parameter to value %ld", v);
	atomic_long_set((atomic_long_t *) kp->arg, v);

	return 0;
}