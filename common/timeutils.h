
#include <stdint.h>
#include <time.h>

#define NS_PER_SEC (uint64_t)1e9

static inline uint64_t
get_time_ns() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}
