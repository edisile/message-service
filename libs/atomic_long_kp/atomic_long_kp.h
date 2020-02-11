#include <asm-generic/atomic-long.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/moduleparam.h>

static int param_long_atomic_set(const char *val, const struct kernel_param *kp);

static const struct kernel_param_ops atomic_long_param_ops = {
	.set = param_long_atomic_set,
	.get = param_get_long,
};

static int param_long_atomic_set(const char *val, const struct kernel_param *kp) {
	long v;
	int ret;

	ret = kstrtol(val, 10, &v);
	if (ret != 0 || v < 0)
	return -EINVAL;
	
	printk("Atomically setting parameter to value %ld", v);
	atomic_long_set((atomic_long_t *) kp->arg, v);

	return 0;
}