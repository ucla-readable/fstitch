#ifndef __INC_MALLOC_H
#define __INC_MALLOC_H

/* Define this symbol if you want to use the special "fail-fast" malloc()
 * implementation to detect bugs. It uses the virtual memory system to make any
 * attempt to access freed memory cause a page fault immediately. However, it is
 * much less space efficient than the default implementation. */
//#define USE_FAILFAST_MALLOC

#include <inc/types.h>
#include <inc/assert.h>
#include <inc/lib.h>

/* SVID2/XPG mallinfo structure */
struct mallinfo {
	int arena;    /* space allocated from system */
	int ordblks;  /* number of free chunks */
	int smblks;   /* number of fastbin blocks */
	int hblks;    /* unused */
	int hblkhd;   /* unused */
	int usmblks;  /* maximum total allocated space */
	int fsmblks;  /* space available in freed fastbin blocks */
	int uordblks; /* total allocated space */
	int fordblks; /* total free space */
	int keepcost; /* top-most, releasable (via malloc_trim) space */
};

/* the standard malloc() functions */
void * malloc(size_t n);
void free(void * p);
void * calloc(size_t n_elements, size_t element_size);
void * realloc(void * p, size_t n);
void * memalign(size_t alignment, size_t n);
void * valloc(size_t n);

/* allocate many chunks of constant size that may be independently freed, guaranteeing that they are contiguous */
void ** independent_calloc(size_t n_elements, size_t size, void * chunks[]);

/* allocate many chunks of varying size that may be independently freed, guaranteeing that they are contiguous */
void ** independent_comalloc(size_t n_elements, size_t * sizes, void * chunks[]);

/* allocate an integral number of pages large enough to hold a specified size */
void * pvalloc(size_t n);

/* releases memory to the system if possible, leaving a specified padding of free space reserved */
int malloc_trim(size_t pad);

/* returns the amount of memory actually allocated at a given location */
size_t malloc_usable_size(void * p);

/* displays memory usage statistics */
void malloc_stats(void);

/* Sets tunable parameters. The format is to provide a (parameter-number,
 * parameter-value) pair. mallopt then sets the corresponding parameter to the
 * argument value if it can (i.e., so long as the value is meaningful), and
 * returns 1 if successful else 0. SVID/XPG/ANSI defines four standard param
 * numbers for mallopt, normally defined in malloc.h. Only one of these
 * (M_MXFAST) is used in this malloc. The others (M_NLBLKS, M_GRAIN, M_KEEP)
 * don't apply, so setting them has no effect. But this malloc also supports
 * four other options in mallopt. See below for details. Briefly, supported
 * parameters are as follows (listed defaults are for "typical" configurations).
 *
 * Symbol            param #   default    allowed param values
 * M_MXFAST          1         64         0-80  (0 disables fastbins)
 * M_TRIM_THRESHOLD -1         32*1024    any   (-1U disables trimming)
 * M_TOP_PAD        -2         0          any  
 */
int mallopt(int parameter_number, int parameter_value);

/* Returns (by copy) a struct containing various summary statistics:
 *
 * arena:     current total bytes allocated from system 
 * ordblks:   the number of free chunks 
 * smblks:    the number of fastbin blocks (i.e., small chunks that have been
 *            freed but not use resused or consolidated)
 * usmblks:   the maximum total allocated space. This will be greater than
 *            current total if trimming has occurred.
 * fsmblks:   total bytes held in fastbin blocks 
 * uordblks:  current total allocated space
 * fordblks:  total free space 
 * keepcost:  the maximum number of bytes that could ideally be released back to
 *            the system via malloc_trim. ("ideally" means that it ignores page
 *            restrictions etc.)
 *
 * Because these fields are ints, but internal bookkeeping may be kept as longs,
 * the reported values may wrap around zero and thus be inaccurate. */
struct mallinfo mallinfo(void);

#endif /* __INC_MALLOC_H */
