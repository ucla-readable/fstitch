#ifndef KUDOS_INC_MATH_H
#define KUDOS_INC_MATH_H

#define M_PI 3.1415926

static __inline double cos(double a) __attribute__((always_inline));
static __inline double
cos(double a)
{
	asm("fcos" : "=t" (a) : "0" (a));
	return a;
}

static __inline double sin(double a) __attribute__((always_inline));
static __inline double
sin(double a)
{
	asm("fsin" : "=t" (a) : "0" (a));
	return a;
}

static __inline double floor(double d) __attribute__((always_inline));
static __inline double
floor(double d)
{
	return (int) ((d >= (int) d) ? d : d - 1);
}

static __inline double ceil(double d) __attribute__((always_inline));
static __inline double
ceil(double d)
{
	return (int) ((d <= (int) d) ? d : d + 1);
}

#endif /* !KUDOS_INC_MATH_H */
