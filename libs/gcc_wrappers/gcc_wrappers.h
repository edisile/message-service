#ifndef GCC_WRAPPERS
	//More conveniently named wrappers for GCC built-ins
	#define GCC_WRAPPERS
	#define __atomic_inc(a) (__sync_fetch_and_add(a, 1))
	#define __atomic_dec(a) (__sync_fetch_and_sub(a, 1))
	#define __atomic_add(ptr, val) (__sync_fetch_and_add(ptr, val))
	#define __atomic_sub(ptr, val) (__sync_fetch_and_sub(ptr, val))
	#define __atomic_swap(ptr, old, new) (__sync_bool_compare_and_swap(ptr, old, new))
#endif