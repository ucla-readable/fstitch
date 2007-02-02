#ifdef __KERNEL_TIMING_H
#error kernel_timing.h should only be included once
#endif

#define __KERNEL_TIMING_H

#if defined(__KERNEL__) && defined(DEBUG_TIMING) && DEBUG_TIMING

#include <linux/types.h>

struct kernel_timing {
	struct timespec total, min, max;
	uint32_t count;
};

struct kernel_interval {
	struct timespec start;
};

static inline void timing_start(struct kernel_interval *) __attribute__((always_inline));
static inline void timing_start(struct kernel_interval * interval)
{
	interval->start = current_kernel_time();
}

static inline void timing_stop(const struct kernel_interval *, struct kernel_timing *) __attribute__((always_inline));
static inline void timing_stop(const struct kernel_interval * interval, struct kernel_timing * timing)
{
	/* calculate interval time */
	struct timespec stop = current_kernel_time();
	stop.tv_nsec -= interval->start.tv_nsec;
	stop.tv_sec -= interval->start.tv_sec;
	if(stop.tv_nsec < 0)
	{
		stop.tv_nsec += NSEC_PER_SEC;
		stop.tv_sec--;
	}
	/* update total */
	timing->total.tv_nsec += stop.tv_nsec;
	timing->total.tv_sec += stop.tv_sec;
	if(timing->total.tv_nsec >= NSEC_PER_SEC)
	{
		timing->total.tv_nsec -= NSEC_PER_SEC;
		timing->total.tv_sec++;
	}
	timing->count++;
	/* update min */
	if(stop.tv_sec < timing->min.tv_sec || (stop.tv_sec == timing->min.tv_sec && stop.tv_nsec < timing->min.tv_nsec))
		timing->min = stop;
	/* update max */
	if(stop.tv_sec > timing->max.tv_sec || (stop.tv_sec == timing->max.tv_sec && stop.tv_nsec > timing->max.tv_nsec))
		timing->max = stop;
}

static inline void timing_dump(const struct kernel_timing *, const char *, const char *) __attribute__((always_inline));
static inline void timing_dump(const struct kernel_timing * timing, const char * name, const char * count)
{
	printk(KERN_ERR "%s: %d %s\n", name, timing->count, count);
	printk(KERN_ERR "%s: total: %ld.%09ld\n", name, timing->total.tv_sec, timing->total.tv_nsec);
	if(timing->count)
	{
		printk(KERN_ERR "%s: min:   %ld.%09ld\n", name, timing->min.tv_sec, timing->min.tv_nsec);
		printk(KERN_ERR "%s: max:   %ld.%09ld\n", name, timing->max.tv_sec, timing->max.tv_nsec);
	}
}

#define KERNEL_TIMING(name) static struct kernel_timing timing_##name = {.total = {0, 0}, .min = {99, 0}, .max = {0, 0}, .count = 0}
#define KERNEL_INTERVAL(name) struct kernel_interval interval_##name
#define TIMING_START(interval) timing_start(&interval_##interval)
#define TIMING_STOP(interval, timing) timing_stop(&interval_##interval, &timing_##timing)
#define TIMING_DUMP(timing, name, count) timing_dump(&timing_##timing, name, count)

#else

#define KERNEL_TIMING(name)
#define KERNEL_INTERVAL(name)
#define TIMING_START(interval)
#define TIMING_STOP(interval, timing)
#define TIMING_DUMP(timing, name, count)

#endif
