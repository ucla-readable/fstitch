/*
  This is a version of malloc originally derived from version 2.7.2 of Doug
  Lea's malloc. Most of the features and portability not required in KudOS have
  been removed to make the code more understandable. The original version can be
  found at:
  ftp://gee.cs.oswego.edu/pub/misc/malloc.c

* Contents, described in more detail in "description of public routines" below.

  Standard (ANSI/SVID/...) functions:
    malloc(size_t n);
    calloc(size_t n_elements, size_t element_size);
    free(void * p);
    realloc(void * p, size_t n);
    memalign(size_t alignment, size_t n);
    valloc(size_t n);
    mallinfo(void)
    mallopt(int parameter_number, int parameter_value)

  Additional functions:
    independent_calloc(size_t n_elements, size_t size, void * chunks[]);
    independent_comalloc(size_t n_elements, size_t * sizes, void * chunks[]);
    pvalloc(size_t n);
    malloc_trim(size_t pad);
    malloc_usable_size(void * p);
    malloc_stats(void);

* Synopsis of compile-time options:

    OPTION                     DEFAULT VALUE

    Compilation Environment options:

    USE_MEMCPY                 1

    Configuration and functionality options:

    DEBUG                      NOT defined
    REALLOC_ZERO_BYTES_FREES   NOT defined
    MALLOC_FAILURE_ACTION      (nothing)
    TRIM_FASTBINS              0
    FIRST_SORTED_BIN_SIZE      512

    Options for customizing MORECORE:

    MORECORE                   sbrk
    MORECORE_CONTIGUOUS        1

    Tuning options that are also dynamically changeable via mallopt:

    DEFAULT_MXFAST             64
    DEFAULT_TRIM_THRESHOLD     32 * 1024
    DEFAULT_TOP_PAD            0

    There are several other #defined constants and macros that you probably
    don't want to touch unless you are extending or adapting malloc.
*/

#include <inc/malloc.h>
#include <inc/types.h>
#include <inc/stdio.h>

/* When tracking down memory leaks it may be helpful to track where memory is
 * allocated, how much is allocated, and the address of the allocated
 * memory and where this memory is freed. Set DEBUG_MEM_LEAK to 1 and
 * then at runtime set malloc_debug = 1 in the env you wish to track
 * to have malloc print this information.
 */
#define DEBUG_MEM_LEAK 0

#if DEBUG_MEM_LEAK
void * __malloc(size_t s);
void   __free(void * x);

bool malloc_debug = 0;

void * malloc(size_t s)
{
	void * x = __malloc(s);
	if (malloc_debug)
	{
		printf("malloc(%u) = 0x%08x, from 0x%08x\n", s, x, __builtin_return_address(0));
		/* It may be helpful to insert code here that int3's to get a
		 * full backtrace. You may find it wise to selectively int3 based
		 * on the return address, s, x, ...
		 */
	}
	return x;
}

void free(void * x)
{
	__free(x);
	if (malloc_debug)
		printf("free(0x%08x), from 0x%08x\n", x, __builtin_return_address(0));
}

#define malloc __malloc
#define free   __free
#endif


#ifndef USE_FAILFAST_MALLOC /* [ */

/*
  Debugging:

  Because freed chunks may be overwritten with bookkeeping fields, this malloc
  will often die when freed memory is overwritten by user programs. This can be
  very effective (albeit in an annoying way) in helping track down dangling
  pointers.

  If you compile with -DDEBUG, a number of assertion checks are enabled that
  will catch more memory errors. You probably won't be able to make much sense
  of the actual assertion errors, but they should help you locate incorrectly
  overwritten memory. The checking is fairly extensive, and will slow down
  execution noticeably. Calling malloc_stats or mallinfo with DEBUG set will
  attempt to check every allocated and free chunk in the course of computing the
  summmaries.

  Setting DEBUG may also be helpful if you are trying to modify this code. The
  assertions in the check routines spell out in more detail the assumptions and
  invariants underlying the algorithms.

  Setting DEBUG does NOT provide an automated mechanism for checking that all
  accesses to malloced memory stay within their bounds. However, there are
  several add-ons and adaptations of this or other mallocs available that do
  this.
*/
#if DEBUG
#define malloc_assert assert
#else
#define malloc_assert(x) ((void) 0)
#endif

/*
  The unsigned integer type used for comparing any two chunk sizes. This should
  be at least as wide as size_t, but should not be signed.
*/
typedef unsigned long chunk_size_t;

/*
  internal_size_t is the word-size used for internal bookkeeping of chunk sizes.
  The default version is the same as size_t.
*/
typedef size_t internal_size_t;

/* The corresponding word size */
#define SIZE_SZ (sizeof(internal_size_t))


/*
  MALLOC_ALIGNMENT is the minimum alignment for malloc'ed chunks. It must be a
  power of two at least 2 * SIZE_SZ, even on machines for which smaller
  alignments would suffice. It may be defined as larger than this though. Note
  however that code and data structures are optimized for the case of 8-byte
  alignment.
*/
#define MALLOC_ALIGNMENT (2 * SIZE_SZ)

/* The corresponding bit mask value */
#define MALLOC_ALIGN_MASK (MALLOC_ALIGNMENT - 1)


/*
  REALLOC_ZERO_BYTES_FREES should be set if a call to realloc with zero bytes
  should be the same as a call to free. Some people think it should. Otherwise,
  since this malloc returns a unique pointer for malloc(0), so does realloc(p, 0).
*/
/* #define REALLOC_ZERO_BYTES_FREES */

/*
  TRIM_FASTBINS controls whether free() of a very small chunk can immediately
  lead to trimming. Setting to true (1) can reduce memory footprint, but will
  almost always slow down programs that use a lot of small chunks.

  Define this only if you are willing to give up some speed to more aggressively
  reduce system-level memory footprint when releasing memory in programs that
  use many small chunks. You can get essentially the same effect by setting
  MXFAST to 0, but this can lead to even greater slowdowns in programs using
  many small chunks. TRIM_FASTBINS is an in-between compile-time option, that
  disables only those chunks bordering topmost memory from being placed in
  fastbins.
*/
#ifndef TRIM_FASTBINS
#define TRIM_FASTBINS 0
#endif


/*
  USE_MEMCPY should be defined if you have memcpy and memset in your C library
  and want to use them in calloc and realloc. Otherwise simple macro versions
  are defined below.

  USE_MEMCPY should be defined as 1 if you actually want to have memset and
  memcpy called. People report that the macro versions are faster than libc
  versions on some systems.

  Even if USE_MEMCPY is set to 1, loops to copy/clear small chunks (of <= 36
  bytes) are manually unrolled in realloc and calloc.
*/
#ifndef USE_MEMCPY
#define USE_MEMCPY 1
#endif

/*
  MALLOC_FAILURE_ACTION is the action to take before "return 0" when malloc
  fails to be able to return memory, either because memory is exhausted or
  because of illegal arguments.

  By default, sets errno if running on STD_C platform, else does nothing.
*/
#ifndef MALLOC_FAILURE_ACTION
#define MALLOC_FAILURE_ACTION
#endif

/*
  MORECORE is the name of the routine to call to obtain more memory from the
  system. See below for general guidance on writing alternative MORECORE
  functions. By default, rely on sbrk.
*/
#ifndef MORECORE
#define MORECORE sbrk
static void * sbrk(size_t);
#endif

/*
  MORECORE_FAILURE is the value returned upon failure of MORECORE. Since it
  cannot be an otherwise valid memory address, and must reflect values of
  standard system calls, you probably ought not try to redefine it.
*/
#ifndef MORECORE_FAILURE
#define MORECORE_FAILURE (-1)
#endif

/*
  If MORECORE_CONTIGUOUS is true, take advantage of fact that consecutive calls
  to MORECORE with positive arguments always return contiguous increasing
  addresses. This is true of unix sbrk. Even if not defined, when regions
  happen to be contiguous, malloc will permit allocations spanning regions
  obtained from different calls. But defining this when applicable enables some
  stronger consistency checks and space efficiencies.
*/
#ifndef MORECORE_CONTIGUOUS
#define MORECORE_CONTIGUOUS 1
#endif

/*
  The system page size. To the extent possible, this malloc manages
  memory from the system in page-size units. Note that this value is
  cached during initialization into a field of malloc_state. So even
  if malloc_getpagesize is a function, it is only called once.
*/
#define malloc_getpagesize PGSIZE

/*
  This version of malloc supports the standard SVID/XPG mallinfo
  routine that returns a struct containing usage properties and
  statistics.

  The main declaration needed is the mallinfo struct that is returned
  (by-copy) by mallinfo(). The SVID/XPG malloinfo struct contains a
  bunch of fields that are not even meaningful in this version of
  malloc. These fields are are instead filled by mallinfo() with
  other numbers that might be of interest.
*/

/*
  SVID/XPG defines four standard parameter numbers for mallopt,
  normally defined in malloc.h.  Only one of these (M_MXFAST) is used
  in this malloc. The others (M_NLBLKS, M_GRAIN, M_KEEP) don't apply,
  so setting them has no effect. But this malloc also supports other
  options in mallopt described below.
*/

/* mallopt tuning options */

/*
  M_MXFAST is the maximum request size used for "fastbins", special bins
  that hold returned chunks without consolidating their spaces. This
  enables future requests for chunks of the same size to be handled
  very quickly, but can increase fragmentation, and thus increase the
  overall memory footprint of a program.

  This malloc manages fastbins very conservatively yet still
  efficiently, so fragmentation is rarely a problem for values less
  than or equal to the default.  The maximum supported value of MXFAST
  is 80. You wouldn't want it any higher than this anyway.  Fastbins
  are designed especially for use with many small structs, objects or
  strings -- the default handles structs/objects/arrays with sizes up
  to 16 4byte fields, or small strings representing words, tokens,
  etc. Using fastbins for larger objects normally worsens
  fragmentation without improving speed.

  M_MXFAST is set in REQUEST size units. It is internally used in
  chunksize units, which adds padding and alignment.  You can reduce
  M_MXFAST to 0 to disable all use of fastbins.  This causes the malloc
  algorithm to be a closer approximation of fifo-best-fit in all cases,
  not just for larger requests, but will generally cause it to be
  slower.
*/

/* M_MXFAST is a standard SVID/XPG tuning option, usually listed in malloc.h */
#ifndef M_MXFAST
#define M_MXFAST 1
#endif

#ifndef DEFAULT_MXFAST
#define DEFAULT_MXFAST 64
#endif


/*
  M_TRIM_THRESHOLD is the maximum amount of unused top-most memory to keep
  before releasing via malloc_trim in free().

  Automatic trimming is mainly useful in long-lived programs. Because trimming
  via sbrk can be slow on some systems, and can sometimes be wasteful (in cases
  where programs immediately afterward allocate more large chunks) the value
  should be high enough so that your overall system performance would improve by
  releasing this much memory.

  The trim value must be greater than page size to have any useful effect. To
  disable trimming completely, you can set it to (unsigned long) (-1)

  Trim settings interact with fastbin (MXFAST) settings: Unless TRIM_FASTBINS is
  defined, automatic trimming never takes place upon freeing a chunk with size
  less than or equal to MXFAST. Trimming is instead delayed until subsequent
  freeing of larger chunks. However, you can still force an attempted trim by
  calling malloc_trim.

  Note that the trick some people use of mallocing a huge space and then freeing
  it at program startup, in an attempt to reserve system memory, doesn't have
  the intended effect under automatic trimming, since that memory will
  immediately be returned to the system.
*/

#define M_TRIM_THRESHOLD (-1)

#ifndef DEFAULT_TRIM_THRESHOLD
#define DEFAULT_TRIM_THRESHOLD (32 * 1024)
#endif

/*
  M_TOP_PAD is the amount of extra `padding' space to allocate or retain
  whenever sbrk is called. It is used in two ways internally:

  * When sbrk is called to extend the top of the arena to satisfy a new malloc
    request, this much padding is added to the sbrk request.

  * When malloc_trim is called automatically from free(), it is used as the
    `pad' argument.

  In both cases, the actual amount of padding is rounded so that the end of the
  arena is always a system page boundary.

  The main reason for using padding is to avoid calling sbrk so often. Having
  even a small pad greatly reduces the likelihood that nearly every malloc
  request during program start-up (or after trimming) will invoke sbrk, which
  needlessly wastes time.

  Automatic rounding-up to page-size units is normally sufficient to avoid
  measurable overhead, so the default is 0. However, in systems where sbrk is
  relatively slow, it can pay to increase this value, at the expense of carrying
  around more memory than the program needs.
*/

#define M_TOP_PAD (-2)

#ifndef DEFAULT_TOP_PAD
#define DEFAULT_TOP_PAD (0)
#endif


/* ------------- Optional versions of memcopy ---------------- */

#if USE_MEMCPY

/*
  Note: memcpy is ONLY invoked with non-overlapping regions,
  so the (usually slower) memmove is not needed.
*/

#define MALLOC_COPY(dest, src, nbytes) memcpy(dest, src, nbytes)
#define MALLOC_ZERO(dest, nbytes) memset(dest, 0, nbytes)

#else /* !USE_MEMCPY */

/* Use Duff's device for good zeroing/copying performance. */

#define MALLOC_ZERO(charp, nbytes)                                \
do {                                                              \
  internal_size_t* mzp = (internal_size_t*)(charp);               \
  chunk_size_t  mctmp = (nbytes)/sizeof(internal_size_t);         \
  long mcn;                                                       \
  if(mctmp < 8) mcn = 0; else { mcn = (mctmp-1)/8; mctmp %= 8; } \
  switch(mctmp) {                                                 \
    case 0: for(;;) { *mzp++ = 0;                                 \
    case 7:           *mzp++ = 0;                                 \
    case 6:           *mzp++ = 0;                                 \
    case 5:           *mzp++ = 0;                                 \
    case 4:           *mzp++ = 0;                                 \
    case 3:           *mzp++ = 0;                                 \
    case 2:           *mzp++ = 0;                                 \
    case 1:           *mzp++ = 0; if(mcn <= 0) break; mcn--; }    \
  }                                                               \
} while(0)

#define MALLOC_COPY(dest,src,nbytes)                                    \
do {                                                                    \
  internal_size_t* mcsrc = (internal_size_t*) src;                      \
  internal_size_t* mcdst = (internal_size_t*) dest;                     \
  chunk_size_t  mctmp = (nbytes)/sizeof(internal_size_t);               \
  long mcn;                                                             \
  if(mctmp < 8) mcn = 0; else { mcn = (mctmp-1)/8; mctmp %= 8; }       \
  switch(mctmp) {                                                       \
    case 0: for(;;) { *mcdst++ = *mcsrc++;                              \
    case 7:           *mcdst++ = *mcsrc++;                              \
    case 6:           *mcdst++ = *mcsrc++;                              \
    case 5:           *mcdst++ = *mcsrc++;                              \
    case 4:           *mcdst++ = *mcsrc++;                              \
    case 3:           *mcdst++ = *mcsrc++;                              \
    case 2:           *mcdst++ = *mcsrc++;                              \
    case 1:           *mcdst++ = *mcsrc++; if(mcn <= 0) break; mcn--; } \
  }                                                                     \
} while(0)

#endif


/*
  -----------------------  Chunk representations -----------------------
*/

/*
  This struct declaration is misleading (but accurate and necessary).
  It declares a "view" into memory allowing access to necessary
  fields at known offsets from a given base. See explanation below.
*/

struct malloc_chunk {
  internal_size_t      prev_size;  /* Size of previous chunk (if free).  */
  internal_size_t      size;       /* Size in bytes, including overhead. */

  struct malloc_chunk * fd;        /* double links -- used only if free. */
  struct malloc_chunk * bk;
};


typedef struct malloc_chunk * mchunkptr;

/*
   malloc_chunk details:

    (The following includes lightly edited explanations by Colin Plumb.)

    Chunks of memory are maintained using a `boundary tag' method as described
    in e.g., Knuth or Standish. (See the paper by Paul Wilson
    ftp://ftp.cs.utexas.edu/pub/garbage/allocsrv.ps for a survey of such
    techniques.) Sizes of free chunks are stored both in the front of each
    chunk and at the end. This makes consolidating fragmented chunks into
    bigger chunks very fast. The size fields also hold bits representing
    whether chunks are free or in use.

    An allocated chunk looks like this:


    chunk-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |             Size of previous chunk, if allocated            | |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |             Size of chunk, in bytes                         |P|
      mem-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |             User data starts here...                          .
            .                                                               .
            .             (malloc_usable_space() bytes)                     .
            .                                                               |
nextchunk-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |             Size of chunk                                     |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+


    Where "chunk" is the front of the chunk for the purpose of most of the
    malloc code, but "mem" is the pointer that is returned to the user.
    "Nextchunk" is the beginning of the next contiguous chunk.

    Chunks always begin on even word boundries, so the mem portion (which is
    returned to the user) is also on an even word boundary, and thus at least
    double-word aligned.

    Free chunks are stored in circular doubly-linked lists, and look like this:

    chunk-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |             Size of previous chunk                            |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    `head:' |             Size of chunk, in bytes                         |P|
      mem-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |             Forward pointer to next chunk in list             |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |             Back pointer to previous chunk in list            |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            |             Unused space (may be 0 bytes long)                .
            .                                                               .
            .                                                               |
nextchunk-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    `foot:' |             Size of chunk, in bytes                           |
            +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    The P (PREV_INUSE) bit, stored in the unused low-order bit of the chunk size
    (which is always a multiple of two words), is an in-use bit for the
    *previous* chunk. If that bit is *clear*, then the word before the current
    chunk size contains the previous chunk size, and can be used to find the
    front of the previous chunk. The very first chunk allocated always has this
    bit set, preventing access to non-existent (or non-owned) memory. If
    prev_inuse is set for any given chunk, then you CANNOT determine the size of
    the previous chunk, and might even get a memory addressing fault when trying
    to do so.

    Note that the `foot' of the current chunk is actually represented as the
    prev_size of the NEXT chunk. This makes it easier to deal with alignments
    etc but can be very confusing when trying to extend or adapt this code.

    The one exception to all this is that the special chunk `top' doesn't bother
    using the trailing size field since there is no next contiguous chunk that
    would have to index off it. After initialization, `top' is forced to always
    exist. If it would become less than MINSIZE bytes long, it is replenished.
*/

/*
  ---------- Size and alignment checks and conversions ----------
*/

/* conversion from malloc headers to user pointers, and back */

#define chunk2mem(p) ((void *) ((char *) (p) + 2 * SIZE_SZ))
#define mem2chunk(mem) ((mchunkptr) ((char *) (mem) - 2 * SIZE_SZ))

/* The smallest possible chunk */
#define MIN_CHUNK_SIZE (sizeof(struct malloc_chunk))

/* The smallest size we can malloc is an aligned minimal chunk */

#define MINSIZE ((chunk_size_t) (((MIN_CHUNK_SIZE + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK)))

/* Check if m has acceptable alignment */

#define aligned_OK(m) (((uintptr_t) (m) & MALLOC_ALIGN_MASK) == 0)


/*
   Check if a request is so large that it would wrap around zero when
   padded and aligned. To simplify some other code, the bound is made
   low enough so that adding MINSIZE will also not wrap around sero.
*/

#define REQUEST_OUT_OF_RANGE(req) ((chunk_size_t) (req) >= (chunk_size_t) (internal_size_t) (-2 * MINSIZE))

/* pad request bytes into a usable size -- internal version */

#define request2size(req) (((req) + SIZE_SZ + MALLOC_ALIGN_MASK < MINSIZE) ? MINSIZE : ((req) + SIZE_SZ + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK)

/*  Same, except also perform argument check */

#define checked_request2size(req, sz) \
  if(REQUEST_OUT_OF_RANGE(req)) \
  { \
    MALLOC_FAILURE_ACTION; \
    return 0; \
  } \
  (sz) = request2size(req);

/*
  --------------- Physical chunk operations ---------------
*/


/* size field is or'ed with PREV_INUSE when previous adjacent chunk in use */
#define PREV_INUSE 0x1

/* extract inuse bit of previous chunk */
#define prev_inuse(p) ((p)->size & PREV_INUSE)


/*
  Bits to mask off when extracting size
*/
#define SIZE_BITS (PREV_INUSE)

/* Get size, ignoring use bits */
#define chunksize(p) ((p)->size & ~(SIZE_BITS))


/* Ptr to next physical malloc_chunk. */
#define next_chunk(p) ((mchunkptr) (((char *)(p)) + ((p)->size & ~PREV_INUSE)))

/* Ptr to previous physical malloc_chunk */
#define prev_chunk(p) ((mchunkptr) (((char *)(p)) - ((p)->prev_size)))

/* Treat space at ptr + offset as a chunk */
#define chunk_at_offset(p, s)  ((mchunkptr) (((char *)(p)) + (s)))

/* extract p's inuse bit */
#define inuse(p) ((((mchunkptr) (((char *)(p)) + ((p)->size & ~PREV_INUSE)))->size) & PREV_INUSE)

/* set/clear chunk as being inuse without otherwise disturbing */
#define set_inuse(p) ((mchunkptr) (((char *)(p)) + ((p)->size & ~PREV_INUSE)))->size |= PREV_INUSE
#define clear_inuse(p) ((mchunkptr) (((char*)(p)) + ((p)->size & ~PREV_INUSE)))->size &= ~(PREV_INUSE)

/* check/set/clear inuse bits in known places */
#define inuse_bit_at_offset(p, s) (((mchunkptr) (((char *)(p)) + (s)))->size & PREV_INUSE)
#define set_inuse_bit_at_offset(p, s) (((mchunkptr) (((char *)(p)) + (s)))->size |= PREV_INUSE)
#define clear_inuse_bit_at_offset(p, s) (((mchunkptr) (((char *)(p)) + (s)))->size &= ~(PREV_INUSE))

/* Set size at head, without disturbing its use bit */
#define set_head_size(p, s) ((p)->size = (((p)->size & PREV_INUSE) | (s)))

/* Set size/use field */
#define set_head(p, s) ((p)->size = (s))

/* Set size at footer (only when chunk is not in use) */
#define set_foot(p, s) (((mchunkptr) ((char *)(p) + (s)))->prev_size = (s))


/*
  -------------------- Internal data structures --------------------

   All internal state is held in an instance of malloc_state defined
   below. There are no other static variables.

   Beware of lots of tricks that minimize the total bookkeeping space
   requirements. The result is a little over 1K bytes (for 4byte
   pointers and size_t.)
*/

/*
  Bins

    An array of bin headers for free chunks. Each bin is doubly linked. The
    bins are approximately proportionally (log) spaced. There are a lot of
    these bins (128). This may look excessive, but works very well in practice.
    Most bins hold sizes that are unusual as malloc request sizes, but are more
    usual for fragments and consolidated sets of chunks, which is what these
    bins hold, so they can be found quickly. All procedures maintain the
    invariant that no consolidated chunk physically borders another one, so each
    chunk in a list is known to be preceeded and followed by either inuse chunks
    or the ends of memory.

    Chunks in bins are kept in size order, with ties going to the approximately
    least recently used chunk. Ordering isn't needed for the small bins, which
    all contain the same-sized chunks, but facilitates best-fit allocation for
    larger chunks. These lists are just sequential. Keeping them in order almost
    never requires enough traversal to warrant using fancier ordered data
    structures.

    Chunks of the same size are linked with the most recently freed at the
    front, and allocations are taken from the back. This results in LRU (FIFO)
    allocation order, which tends to give each chunk an equal opportunity to be
    consolidated with adjacent freed chunks, resulting in larger free chunks and
    less fragmentation.

    To simplify use in double-linked lists, each bin header acts as a
    malloc_chunk. This avoids special-casing for headers. But to conserve space
    and improve locality, we allocate only the fd/bk pointers of bins, and then
    use repositioning tricks to treat these as the fields of a malloc_chunk *.
*/

typedef struct malloc_chunk * mbinptr;

/* addressing -- note that bin_at(0) does not exist */
#define bin_at(m, i) ((mbinptr) ((char *) &((m)->bins[(i) << 1]) - (SIZE_SZ << 1)))

/* analog of ++bin */
#define next_bin(b) ((mbinptr) ((char *) (b) + (sizeof(mchunkptr) << 1)))

/* Reminders about list directionality within bins */
#define first(b) ((b)->fd)
#define last(b) ((b)->bk)

/* Take a chunk off a bin list */
#define unlink(P, BK, FD) do { FD = P->fd; BK = P->bk; FD->bk = BK; BK->fd = FD; } while(0)

/*
  Indexing

    Bins for sizes < 512 bytes contain chunks of all the same size, spaced 8
    bytes apart. Larger bins are approximately logarithmically spaced:

    64 bins of size       8
    32 bins of size      64
    16 bins of size     512
     8 bins of size    4096
     4 bins of size   32768
     2 bins of size  262144
     1 bin  of size what's left

    The bins top out around 1MB.
*/

#define NBINS 96
#define NSMALLBINS 32
#define SMALLBIN_WIDTH 8
#define MIN_LARGE_SIZE 256

#define in_smallbin_range(sz) ((chunk_size_t) (sz) < (chunk_size_t) MIN_LARGE_SIZE)

#define smallbin_index(sz) (((unsigned int) (sz)) >> 3)

/*
  Compute index for size. We expect this to be inlined when
  compiled with optimization, else not, which works out well.
*/
static int largebin_index(unsigned int sz)
{
  unsigned int x = sz >> SMALLBIN_WIDTH;
  unsigned int m; /* bit position of highest set bit of m */

  if(x >= 0x10000)
    return NBINS - 1;

#if defined(__GNUC__) && defined(i386)
  /* On x86, use BSRL instruction to find highest bit */
  __asm__("bsrl %1,%0\n\t" : "=r" (m) : "g" (x));
#else
  {
    /*
      Based on branch-free nlz algorithm in chapter 5 of Henry S. Warren Jr's
      book "Hacker's Delight".
    */
    unsigned int n = ((x - 0x100) >> 16) & 8;
    x <<= n;
    m = ((x - 0x1000) >> 16) & 4;
    n += m;
    x <<= m;
    m = ((x - 0x4000) >> 16) & 2;
    n += m;
    x = (x << m) >> 14;
    m = 13 - n + (x & ~(x >> 1));
  }
#endif

  /* Use next 2 bits to create finer-granularity bins */
  return NSMALLBINS + (m << 2) + ((sz >> (m + 6)) & 3);
}

#define bin_index(sz) ((in_smallbin_range(sz)) ? smallbin_index(sz) : largebin_index(sz))

/*
  FIRST_SORTED_BIN_SIZE is the chunk size corresponding to the first bin that is
  maintained in sorted order. This must be the smallest size corresponding to a
  given bin.

  Normally, this should be MIN_LARGE_SIZE. But you can weaken best fit
  guarantees to sometimes speed up malloc by increasing value.  Doing this means
  that malloc may choose a chunk that is non-best-fitting by up to the width of
  the bin.

  Some useful cutoff values:
      512 - all bins sorted
     2560 - leaves bins <=     64 bytes wide unsorted
    12288 - leaves bins <=    512 bytes wide unsorted
    65536 - leaves bins <=   4096 bytes wide unsorted
   262144 - leaves bins <=  32768 bytes wide unsorted
       -1 - no bins sorted (not recommended!)
*/

#define FIRST_SORTED_BIN_SIZE MIN_LARGE_SIZE
/* #define FIRST_SORTED_BIN_SIZE 65536 */

/*
  Unsorted chunks

    All remainders from chunk splits, as well as all returned chunks, are first
    placed in the "unsorted" bin. They are then placed in regular bins after
    malloc gives them ONE chance to be used before binning. So, basically, the
    unsorted_chunks list acts as a queue, with chunks being placed on it in free
    (and malloc_consolidate), and taken off (to be either used or placed in
    bins) in malloc.
*/

/* The otherwise unindexable 1-bin is used to hold unsorted chunks. */
#define unsorted_chunks(M) (bin_at(M, 1))

/*
  Top

    The top-most available chunk (i.e., the one bordering the end of available
    memory) is treated specially. It is never included in any bin, is used only
    if no other chunk is available, and is released back to the system if it is
    very large (see M_TRIM_THRESHOLD). Because top initially points to its own
    bin with initial zero size, thus forcing extension on the first malloc
    request, we avoid having any special code in malloc to check whether it even
    exists yet. But we still need to do so when getting memory from system, so
    we make initial_top treat the bin as a legal but unusable chunk during the
    interval between initialization and the first call to sysmalloc. (This is
    somewhat delicate, since it relies on the 2 preceding words to be zero
    during this interval as well.)
*/

/* Conveniently, the unsorted bin can be used as dummy top on first call */
#define initial_top(M) (unsorted_chunks(M))

/*
  Binmap

    To help compensate for the large number of bins, a one-level index structure
    is used for bin-by-bin searching. `binmap' is a bitvector recording whether
    bins are definitely empty so they can be skipped over during during
    traversals. The bits are NOT always cleared as soon as bins are empty, but
    instead only when they are noticed to be empty during traversal in malloc.
*/

/* Conservatively use 32 bits per map word, even if on 64bit system */
#define BINMAPSHIFT 5
#define BITSPERMAP (1U << BINMAPSHIFT)
#define BINMAPSIZE (NBINS / BITSPERMAP)

#define idx2block(i) ((i) >> BINMAPSHIFT)
#define idx2bit(i) ((1U << ((i) & ((1U << BINMAPSHIFT) - 1))))

#define mark_bin(m,i) ((m)->binmap[idx2block(i)] |= idx2bit(i))
#define unmark_bin(m,i) ((m)->binmap[idx2block(i)] &= ~(idx2bit(i)))
#define get_binmap(m,i) ((m)->binmap[idx2block(i)] & idx2bit(i))

/*
  Fastbins

    An array of lists holding recently freed small chunks. Fastbins are not
    doubly linked. It is faster to single-link them, and since chunks are never
    removed from the middles of these lists, double linking is not necessary.
    Also, unlike regular bins, they are not even processed in FIFO order (they
    use faster LIFO) since ordering doesn't much matter in the transient
    contexts in which fastbins are normally used.

    Chunks in fastbins keep their inuse bit set, so they cannot be consolidated
    with other free chunks. malloc_consolidate releases all chunks in fastbins
    and consolidates them with other free chunks.
*/

typedef struct malloc_chunk * mfastbinptr;

/* offset 2 to use otherwise unindexable first 2 bins */
#define fastbin_index(sz) ((((unsigned int) (sz)) >> 3) - 2)

/* The maximum fastbin request size we support */
#define MAX_FAST_SIZE 80

#define NFASTBINS (fastbin_index(request2size(MAX_FAST_SIZE)) + 1)

/*
  FASTBIN_CONSOLIDATION_THRESHOLD is the size of a chunk in free() that triggers
  automatic consolidation of possibly-surrounding fastbin chunks. This is a
  heuristic, so the exact value should not matter too much. It is defined at
  half the default trim threshold as a compromise heuristic to only attempt
  consolidation if it is likely to lead to trimming. However, it is not
  dynamically tunable, since consolidation reduces fragmentation surrounding
  loarge chunks even if trimming is not used.
*/

#define FASTBIN_CONSOLIDATION_THRESHOLD ((unsigned long) (DEFAULT_TRIM_THRESHOLD) >> 1)

/*
  Since the lowest 2 bits in max_fast don't matter in size comparisons, they are
  used as flags.
*/

/*
  ANYCHUNKS_BIT held in max_fast indicates that there may be any freed chunks at
  all. It is set true when entering a chunk into any bin.
*/

#define ANYCHUNKS_BIT (1U)

#define have_anychunks(M) (((M)->max_fast & ANYCHUNKS_BIT))
#define set_anychunks(M) ((M)->max_fast |= ANYCHUNKS_BIT)
#define clear_anychunks(M) ((M)->max_fast &= ~ANYCHUNKS_BIT)

/*
  FASTCHUNKS_BIT held in max_fast indicates that there are probably
  some fastbin chunks. It is set true on entering a chunk into any
  fastbin, and cleared only in malloc_consolidate.
*/

#define FASTCHUNKS_BIT (2U)

#define have_fastchunks(M) (((M)->max_fast & FASTCHUNKS_BIT))
#define set_fastchunks(M) ((M)->max_fast |= (FASTCHUNKS_BIT|ANYCHUNKS_BIT))
#define clear_fastchunks(M) ((M)->max_fast &= ~(FASTCHUNKS_BIT))

/*
   Set value of max_fast. Use impossibly small value if 0.
*/

#define set_max_fast(M, s) (M)->max_fast = (((s) == 0) ? SMALLBIN_WIDTH: request2size(s)) | ((M)->max_fast & (FASTCHUNKS_BIT | ANYCHUNKS_BIT))
#define get_max_fast(M) ((M)->max_fast & ~(FASTCHUNKS_BIT | ANYCHUNKS_BIT))


/*
  morecore_properties is a status word holding dynamically discovered or
  controlled properties of the morecore function
*/

#define MORECORE_CONTIGUOUS_BIT  (1U)

#define contiguous(M) (((M)->morecore_properties & MORECORE_CONTIGUOUS_BIT))
#define noncontiguous(M) (((M)->morecore_properties & MORECORE_CONTIGUOUS_BIT) == 0)
#define set_contiguous(M) ((M)->morecore_properties |= MORECORE_CONTIGUOUS_BIT)
#define set_noncontiguous(M) ((M)->morecore_properties &= ~MORECORE_CONTIGUOUS_BIT)


/*
   ----------- Internal state representation and initialization -----------
*/

struct malloc_state {
  /* The maximum chunk size to be eligible for fastbin */
  internal_size_t max_fast;   /* low 2 bits used as flags */

  /* Fastbins */
  mfastbinptr fastbins[NFASTBINS];

  /* Base of the topmost chunk -- not otherwise kept in a bin */
  mchunkptr top;

  /* The remainder from the most recent split of a small request */
  mchunkptr last_remainder;

  /* Normal bins packed as described above */
  mchunkptr bins[NBINS * 2];

  /* Bitmap of bins. Trailing zero map handles cases of largest binned size */
  unsigned int binmap[BINMAPSIZE + 1];

  /* Tunable parameters */
  chunk_size_t trim_threshold;
  internal_size_t top_pad;

  /* Cache malloc_getpagesize */
  unsigned int pagesize;

  /* Track properties of MORECORE */
  unsigned int morecore_properties;

  /* Statistics */
  internal_size_t sbrked_mem;
  internal_size_t max_sbrked_mem;
  internal_size_t max_total_mem;
};

typedef struct malloc_state * mstate;

/*
   There is exactly one instance of this struct in this malloc. If you are
   adapting this malloc in a way that does NOT use a static malloc_state, you
   MUST explicitly zero-fill it before using. This malloc relies on the property
   that malloc_state is initialized to all zeroes (as is true of C statics).
*/

static struct malloc_state av_;  /* never directly referenced */

/*
   All uses of av_ are via get_malloc_state().
   At most one "call" to get_malloc_state is made per invocation of the public
   versions of malloc and free, but other routines that in turn invoke malloc
   and/or free may call more then once. Also, it is called in check* routines
   if DEBUG is set.
*/

#define get_malloc_state() (&(av_))

/*
  Initialize a malloc_state struct.

  This is called only from within malloc_consolidate, which needs be called in
  the same contexts anyway. It is never called directly outside of
  malloc_consolidate because some optimizing compilers try to inline it at all
  call points, which turns out not to be an optimization at all. (Inlining it in
  malloc_consolidate is fine though.)
*/

static void malloc_init_state(mstate av)
{
  int     i;
  mbinptr bin;

  /* Establish circular links for normal bins */
  for(i = 1; i < NBINS; ++i)
  {
    bin = bin_at(av,i);
    bin->fd = bin->bk = bin;
  }

  av->top_pad        = DEFAULT_TOP_PAD;
  av->trim_threshold = DEFAULT_TRIM_THRESHOLD;

#if MORECORE_CONTIGUOUS
  set_contiguous(av);
#else
  set_noncontiguous(av);
#endif

  set_max_fast(av, DEFAULT_MXFAST);

  av->top            = initial_top(av);
  av->pagesize       = malloc_getpagesize;
}

/*
   Other internal utilities operating on mstates
*/

static void * sysmalloc(internal_size_t, mstate);
static int systrim(size_t, mstate);
static void malloc_consolidate(mstate);
static void ** independent_alloc(size_t, size_t *, int, void **);

/*
  Debugging support

  These routines make a number of assertions about the states of data structures
  that should be true at all times. If any are not true, it's very likely that a
  user program has somehow trashed memory. (It's also possible that there is a
  coding error in malloc. In which case, please report it!)
*/

#if ! DEBUG
#define check_chunk(P)
#define check_free_chunk(P)
#define check_inuse_chunk(P)
#define check_remalloced_chunk(P,N)
#define check_malloced_chunk(P,N)
#define check_malloc_state()
#else

#define check_chunk(P)              do_check_chunk(P)
#define check_free_chunk(P)         do_check_free_chunk(P)
#define check_inuse_chunk(P)        do_check_inuse_chunk(P)
#define check_remalloced_chunk(P,N) do_check_remalloced_chunk(P,N)
#define check_malloced_chunk(P,N)   do_check_malloced_chunk(P,N)
#define check_malloc_state()        do_check_malloc_state()

/*
  Properties of all chunks
*/
static void do_check_chunk(mchunkptr p)
{
  mstate av = get_malloc_state();
  chunk_size_t sz = chunksize(p);
  /* min and max possible addresses assuming contiguous allocation */
  char * max_address = (char *) (av->top) + chunksize(av->top);
  char * min_address = max_address - av->sbrked_mem;

  /* Has legal address ... */
  if(p != av->top)
  {
    if(contiguous(av))
    {
      malloc_assert(((char *) p) >= min_address);
      malloc_assert(((char *) p + sz) <= ((char * ) (av->top)));
    }
  }
  else
  {
    /* top size is always at least MINSIZE */
    malloc_assert((chunk_size_t) sz >= MINSIZE);
    /* top predecessor always marked inuse */
    malloc_assert(prev_inuse(p));
  }
}

/*
  Properties of free chunks
*/
static void do_check_free_chunk(mchunkptr p)
{
  mstate av = get_malloc_state();

  internal_size_t sz = p->size & ~PREV_INUSE;
  mchunkptr next = chunk_at_offset(p, sz);

  do_check_chunk(p);

  /* Chunk must claim to be free ... */
  malloc_assert(!inuse(p));

  /* Unless a special marker, must have OK fields */
  if((chunk_size_t) sz >= MINSIZE)
  {
    malloc_assert((sz & MALLOC_ALIGN_MASK) == 0);
    malloc_assert(aligned_OK(chunk2mem(p)));
    /* ... matching footer field */
    malloc_assert(next->prev_size == sz);
    /* ... and is fully consolidated */
    malloc_assert(prev_inuse(p));
    malloc_assert(next == av->top || inuse(next));

    /* ... and has minimally sane links */
    malloc_assert(p->fd->bk == p);
    malloc_assert(p->bk->fd == p);
  }
  else /* markers are always of size SIZE_SZ */
    malloc_assert(sz == SIZE_SZ);
}

/*
  Properties of inuse chunks
*/
static void do_check_inuse_chunk(mchunkptr p)
{
  mstate av = get_malloc_state();
  mchunkptr next;
  do_check_chunk(p);

  /* Check whether it claims to be in use ... */
  malloc_assert(inuse(p));

  next = next_chunk(p);

  /* ... and is surrounded by OK chunks.
    Since more things can be checked with free chunks than inuse ones,
    if an inuse chunk borders them and debug is on, it's worth doing them.
  */
  if(!prev_inuse(p))
  {
    /* Note that we cannot even look at prev unless it is not inuse */
    mchunkptr prv = prev_chunk(p);
    malloc_assert(next_chunk(prv) == p);
    do_check_free_chunk(prv);
  }

  if(next == av->top)
  {
    malloc_assert(prev_inuse(next));
    malloc_assert(chunksize(next) >= MINSIZE);
  }
  else if(!inuse(next))
    do_check_free_chunk(next);
}

/*
  Properties of chunks recycled from fastbins
*/
static void do_check_remalloced_chunk(mchunkptr p, internal_size_t s)
{
  internal_size_t sz = p->size & ~PREV_INUSE;

  do_check_inuse_chunk(p);

  /* Legal size ... */
  malloc_assert((sz & MALLOC_ALIGN_MASK) == 0);
  malloc_assert((chunk_size_t) sz >= MINSIZE);
  /* ... and alignment */
  malloc_assert(aligned_OK(chunk2mem(p)));
  /* chunk is less than MINSIZE more than request */
  malloc_assert((long) sz - (long) s >= 0);
  malloc_assert((long) sz - (long) (s + MINSIZE) < 0);
}

/*
  Properties of nonrecycled chunks at the point they are malloced
*/
static void do_check_malloced_chunk(mchunkptr p, internal_size_t s)
{
  /* same as recycled case ... */
  do_check_remalloced_chunk(p, s);

  /*
    ... plus, must obey implementation invariant that prev_inuse is always true
    of any allocated chunk; i.e., that each allocated chunk borders either a
    previously allocated and still in-use chunk, or the base of its memory
    arena. This is ensured by making all allocations from the the `lowest' part
    of any found chunk. This does not necessarily hold however for chunks
    recycled via fastbins.
  */

  malloc_assert(prev_inuse(p));
}


/*
  Properties of malloc_state.

  This may be useful for debugging malloc, as well as detecting user
  programmer errors that somehow write into malloc_state.

  If you are extending or experimenting with this malloc, you can
  probably figure out how to hack this routine to print out or
  display chunk addresses, sizes, bins, and other instrumentation.
*/
static void do_check_malloc_state()
{
  mstate av = get_malloc_state();
  int i;
  mchunkptr p;
  mchunkptr q;
  mbinptr b;
  unsigned int binbit;
  int empty;
  unsigned int idx;
  internal_size_t size;
  chunk_size_t total = 0;
  int max_fast_bin;

  /* internal size_t must be no wider than pointer type */
  malloc_assert(sizeof(internal_size_t) <= sizeof(char *));

  /* alignment is a power of 2 */
  malloc_assert((MALLOC_ALIGNMENT & (MALLOC_ALIGNMENT - 1)) == 0);

  /* cannot run remaining checks until fully initialized */
  if(av->top == 0 || av->top == initial_top(av))
    return;

  /* pagesize is a power of 2 */
  malloc_assert((av->pagesize & (av->pagesize - 1)) == 0);

  /* properties of fastbins */

  /* max_fast is in allowed range */
  malloc_assert(get_max_fast(av) <= request2size(MAX_FAST_SIZE));

  max_fast_bin = fastbin_index(av->max_fast);

  for(i = 0; i < NFASTBINS; ++i)
  {
    p = av->fastbins[i];

    /* all bins past max_fast are empty */
    if(i > max_fast_bin)
      malloc_assert(p == 0);

    while(p != 0)
    {
      /* each chunk claims to be inuse */
      do_check_inuse_chunk(p);
      total += chunksize(p);
      /* chunk belongs in this bin */
      malloc_assert(fastbin_index(chunksize(p)) == i);
      p = p->fd;
    }
  }

  if(total != 0)
    malloc_assert(have_fastchunks(av));
  else if(!have_fastchunks(av))
    malloc_assert(total == 0);

  /* check normal bins */
  for(i = 1; i < NBINS; ++i)
  {
    b = bin_at(av,i);

    /* binmap is accurate (except for bin 1 == unsorted_chunks) */
    if(i >= 2)
    {
      binbit = get_binmap(av,i);
      empty = last(b) == b;
      if(!binbit)
        malloc_assert(empty);
      else if(!empty)
        malloc_assert(binbit);
    }

    for(p = last(b); p != b; p = p->bk)
    {
      /* each chunk claims to be free */
      do_check_free_chunk(p);
      size = chunksize(p);
      total += size;
      if(i >= 2)
      {
        /* chunk belongs in bin */
        idx = bin_index(size);
        malloc_assert(idx == i);
        /* lists are sorted */
        if((chunk_size_t) size >= (chunk_size_t)(FIRST_SORTED_BIN_SIZE))
          malloc_assert(p->bk == b || (chunk_size_t) chunksize(p->bk) >= (chunk_size_t) chunksize(p));
      }
      /* chunk is followed by a legal chain of inuse chunks */
      for(q = next_chunk(p); (q != av->top && inuse(q) && (chunk_size_t) (chunksize(q)) >= MINSIZE); q = next_chunk(q))
        do_check_inuse_chunk(q);
    }
  }

  /* top chunk is OK */
  check_chunk(av->top);

  /* sanity checks for statistics */

  malloc_assert(total <= (chunk_size_t) (av->max_total_mem));
  malloc_assert((chunk_size_t) (av->sbrked_mem) <= (chunk_size_t) (av->max_sbrked_mem));
  malloc_assert((chunk_size_t) (av->max_total_mem) >= (chunk_size_t) (av->sbrked_mem));
}
#endif


/* ----------- Routines dealing with system allocation -------------- */

/*
  sysmalloc handles malloc cases requiring more memory from the system.
  On entry, it is assumed that av->top does not have enough
  space to service request for nb bytes, thus requiring that av->top
  be extended or replaced.
*/

static void * sysmalloc(internal_size_t nb, mstate av)
{
  mchunkptr       old_top;        /* incoming value of av->top */
  internal_size_t old_size;       /* its size */
  char *          old_end;        /* its end address */

  long            size;           /* arg to first MORECORE call */
  char *          brk;            /* return value from MORECORE */

  long            correction;     /* arg to 2nd MORECORE call */
  char *          snd_brk;        /* 2nd return val */

  internal_size_t front_misalign; /* unusable bytes at front of new space */
  internal_size_t end_misalign;   /* partial page left at end of new space */
  char *          aligned_brk;    /* aligned offset into brk */

  mchunkptr       p;              /* the allocated/returned chunk */
  mchunkptr       remainder;      /* remainder from allocation */
  chunk_size_t    remainder_size; /* its size */

  chunk_size_t    sum;            /* for updating stats */

  size_t          pagemask  = av->pagesize - 1;

  /*
    If there is space available in fastbins, consolidate and retry
    malloc from scratch rather than getting memory from system.  This
    can occur only if nb is in smallbin range so we didn't consolidate
    upon entry to malloc. It is much easier to handle this case here
    than in malloc proper.
  */

  if(have_fastchunks(av))
  {
    malloc_assert(in_smallbin_range(nb));
    malloc_consolidate(av);
    return malloc(nb - MALLOC_ALIGN_MASK);
  }

  /* Record incoming configuration of top */

  old_top  = av->top;
  old_size = chunksize(old_top);
  old_end  = (char *) (chunk_at_offset(old_top, old_size));

  brk = snd_brk = (char*) (MORECORE_FAILURE);

  /*
     If not the first time through, we require old_size to be
     at least MINSIZE and to have prev_inuse set.
  */

  malloc_assert((old_top == initial_top(av) && old_size == 0) || ((chunk_size_t) (old_size) >= MINSIZE && prev_inuse(old_top)));

  /* Precondition: not enough current space to satisfy nb request */
  malloc_assert((chunk_size_t) (old_size) < (chunk_size_t) (nb + MINSIZE));

  /* Precondition: all fastbins are consolidated */
  malloc_assert(!have_fastchunks(av));

  /* Request enough space for nb + pad + overhead */
  size = nb + av->top_pad + MINSIZE;

  /*
    If contiguous, we can subtract out existing space that we hope to
    combine with new space. We add it back later only if
    we don't actually get contiguous space.
  */
  if(contiguous(av))
    size -= old_size;

  /*
    Round to a multiple of page size.
    If MORECORE is not contiguous, this ensures that we only call it
    with whole-page arguments.  And if MORECORE is contiguous and
    this is not first time through, this preserves page-alignment of
    previous calls. Otherwise, we correct to page-align below.
  */
  size = (size + pagemask) & ~pagemask;

  /*
    Don't try to call MORECORE if argument is so big as to appear
    negative.
  */
  if(size > 0)
    brk = (char *) MORECORE(size);

  if(brk != (char*) MORECORE_FAILURE)
  {
    av->sbrked_mem += size;

    /*
      If MORECORE extends previous space, we can likewise extend top size.
    */
    if(brk == old_end && snd_brk == (char*)(MORECORE_FAILURE))
      set_head(old_top, (size + old_size) | PREV_INUSE);
    /*
      Otherwise, make adjustments:

      * If the first time through or noncontiguous, we need to call sbrk
        just to find out where the end of memory lies.

      * We need to ensure that all returned chunks from malloc will meet
        MALLOC_ALIGNMENT

      * If there was an intervening foreign sbrk, we need to adjust sbrk
        request size to account for fact that we will not be able to
        combine new space with existing space in old_top.

      * Almost all systems internally allocate whole pages at a time, in
        which case we might as well use the whole last page of request.
        So we allocate enough more memory to hit a page boundary now,
        which in turn causes future contiguous calls to page-align.
    */
    else
    {
      front_misalign = 0;
      end_misalign = 0;
      correction = 0;
      aligned_brk = brk;

      /*
        If MORECORE returns an address lower than we have seen before,
        we know it isn't really contiguous.  This and some subsequent
        checks help cope with non-conforming MORECORE functions and
        the presence of "foreign" calls to MORECORE from outside of
        malloc or by other threads.  We cannot guarantee to detect
        these in all cases, but cope with the ones we do detect.
      */
      if(contiguous(av) && old_size != 0 && brk < old_end)
        set_noncontiguous(av);

      /* handle contiguous cases */
      if(contiguous(av))
      {
        /*
           We can tolerate forward non-contiguities here (usually due
           to foreign calls) but treat them as part of our space for
           stats reporting.
        */
        if(old_size != 0)
          av->sbrked_mem += brk - old_end;

        /* Guarantee alignment of first new chunk made from this space */

        front_misalign = (internal_size_t) chunk2mem(brk) & MALLOC_ALIGN_MASK;
        if(front_misalign > 0)
        {
          /*
            Skip over some bytes to arrive at an aligned position.
            We don't need to specially mark these wasted front bytes.
            They will never be accessed anyway because
            prev_inuse of av->top (and any chunk created from its start)
            is always true after initialization.
          */

          correction = MALLOC_ALIGNMENT - front_misalign;
          aligned_brk += correction;
        }

        /*
          If this isn't adjacent to existing space, then we will not
          be able to merge with old_top space, so must add to 2nd request.
        */
        correction += old_size;

        /* Extend the end address to hit a page boundary */
        end_misalign = (internal_size_t) (brk + size + correction);
        correction += ((end_misalign + pagemask) & ~pagemask) - end_misalign;

        malloc_assert(correction >= 0);
        snd_brk = (char *) MORECORE(correction);

        if(snd_brk == (char*) MORECORE_FAILURE)
        {
          /*
            If can't allocate correction, try to at least find out current
            brk.  It might be enough to proceed without failing.
          */
          correction = 0;
          snd_brk = (char *) MORECORE(0);
        }
        else if(snd_brk < brk)
        {
          /*
            If the second call gives noncontiguous space even though
            it says it won't, the only course of action is to ignore
            results of second call, and conservatively estimate where
            the first call left us. Also set noncontiguous, so this
            won't happen again, leaving at most one hole.

            Note that this check is intrinsically incomplete.  Because
            MORECORE is allowed to give more space than we ask for,
            there is no reliable way to detect a noncontiguity
            producing a forward gap for the second call.
          */
          snd_brk = brk + size;
          correction = 0;
          set_noncontiguous(av);
        }

      }
      /* handle non-contiguous cases */
      else
      {
        /* MORECORE must correctly align */
        malloc_assert(aligned_OK(chunk2mem(brk)));

        /* Find out current end of memory */
        if(snd_brk == (char *) MORECORE_FAILURE)
        {
          snd_brk = (char *) MORECORE(0);
          av->sbrked_mem += snd_brk - brk - size;
        }
      }

      /* Adjust top based on results of second sbrk */
      if(snd_brk != (char *) MORECORE_FAILURE)
      {
        av->top = (mchunkptr) aligned_brk;
        set_head(av->top, (snd_brk - aligned_brk + correction) | PREV_INUSE);
        av->sbrked_mem += correction;

        /*
          If not the first time through, we either have a
          gap due to foreign sbrk or a non-contiguous region.  Insert a
          double fencepost at old_top to prevent consolidation with space
          we don't own. These fenceposts are artificial chunks that are
          marked as inuse and are in any case too small to use.  We need
          two to make sizes and alignments work out.
        */

        if(old_size != 0)
        {
          /*
             Shrink old_top to insert fenceposts, keeping size a
             multiple of MALLOC_ALIGNMENT. We know there is at least
             enough space in old_top to do this.
          */
          old_size = (old_size - 3 * SIZE_SZ) & ~MALLOC_ALIGN_MASK;
          set_head(old_top, old_size | PREV_INUSE);

          /*
            Note that the following assignments completely overwrite
            old_top when old_size was previously MINSIZE.  This is
            intentional. We need the fencepost, even if old_top otherwise gets
            lost.
          */
          chunk_at_offset(old_top, old_size)->size = SIZE_SZ | PREV_INUSE;
          chunk_at_offset(old_top, old_size + SIZE_SZ)->size = SIZE_SZ | PREV_INUSE;

          /*
             If possible, release the rest, suppressing trimming.
          */
          if(old_size >= MINSIZE)
          {
            internal_size_t tt = av->trim_threshold;
            av->trim_threshold = (internal_size_t) (-1);
            free(chunk2mem(old_top));
            av->trim_threshold = tt;
          }
        }
      }
    }

    /* Update statistics */
    sum = av->sbrked_mem;
    if(sum > (chunk_size_t) av->max_sbrked_mem)
      av->max_sbrked_mem = sum;

    if(sum > (chunk_size_t) av->max_total_mem)
      av->max_total_mem = sum;

    check_malloc_state();

    /* finally, do the allocation */

    p = av->top;
    size = chunksize(p);

    /* check that one of the above allocation paths succeeded */
    if((chunk_size_t) size >= (chunk_size_t) (nb + MINSIZE))
    {
      remainder_size = size - nb;
      remainder = chunk_at_offset(p, nb);
      av->top = remainder;
      set_head(p, nb | PREV_INUSE);
      set_head(remainder, remainder_size | PREV_INUSE);
      check_malloced_chunk(p, nb);
      return chunk2mem(p);
    }

  }

  /* catch all failure paths */
  MALLOC_FAILURE_ACTION;
  return 0;
}


/*
  systrim is an inverse of sorts to sysmalloc. It gives memory back to the
  system (via negative arguments to sbrk) if there is unused memory at the
  `high' end of the malloc pool. It is called automatically by free() when top
  space exceeds the trim threshold. It is also called by the public malloc_trim
  routine. It returns 1 if it actually released any memory, else 0.
*/

static int systrim(size_t pad, mstate av)
{
  long top_size;        /* Amount of top-most memory */
  long extra;           /* Amount to release */
  long released;        /* Amount actually released */
  char * current_brk;   /* address returned by pre-check sbrk call */
  char * new_brk;       /* address returned by post-check sbrk call */
  size_t pagesz;

  pagesz = av->pagesize;
  top_size = chunksize(av->top);

  /* Release in pagesize units, keeping at least one page */
  extra = ((top_size - pad - MINSIZE + (pagesz-1)) / pagesz - 1) * pagesz;

  if(extra > 0)
  {
    /*
      Only proceed if end of memory is where we last set it.
      This avoids problems if there were foreign sbrk calls.
    */
    current_brk = (char *) MORECORE(0);
    if(current_brk == (char *) av->top + top_size)
    {
      /*
        Attempt to release memory. We ignore MORECORE return value,
        and instead call again to find out where new end of memory is.
        This avoids problems if first call releases less than we asked,
        of if failure somehow altered brk value. (We could still
        encounter problems if it altered brk in some very bad way,
        but the only thing we can do is adjust anyway, which will cause
        some downstream failure.)
      */

      MORECORE(-extra);
      new_brk = (char *) MORECORE(0);

      if(new_brk != (char *) MORECORE_FAILURE)
      {
        released = (long) (current_brk - new_brk);

        if(released != 0)
        {
          /* Success. Adjust top. */
          av->sbrked_mem -= released;
          set_head(av->top, (top_size - released) | PREV_INUSE);
          check_malloc_state();
          return 1;
        }
      }
    }
  }
  return 0;
}


void * malloc(size_t bytes)
{
  mstate av = get_malloc_state();

  internal_size_t nb;               /* normalized request size */
  unsigned int    idx;              /* associated bin index */
  mbinptr         bin;              /* associated bin */
  mfastbinptr *   fb;               /* associated fastbin */

  mchunkptr       victim;           /* inspected/selected chunk */
  internal_size_t size;             /* its size */
  int             victim_index;     /* its bin index */

  mchunkptr       remainder;        /* remainder from a split */
  chunk_size_t    remainder_size;   /* its size */

  unsigned int    block;            /* bit map traverser */
  unsigned int    bit;              /* bit map traverser */
  unsigned int    map;              /* current word of binmap */

  mchunkptr       fwd;              /* misc temp for linking */
  mchunkptr       bck;              /* misc temp for linking */

  /*
    Convert request size to internal form by adding SIZE_SZ bytes overhead plus
    possibly more to obtain necessary alignment and/or to obtain a size of at
    least MINSIZE, the smallest allocatable size. Also, checked_request2size
    traps (returning 0) request sizes that are so large that they wrap around
    zero when padded and aligned.
  */
  checked_request2size(bytes, nb);

  /*
    Bypass search if no frees yet
   */
  if(!have_anychunks(av))
  {
    if(av->max_fast == 0) /* initialization check */
      malloc_consolidate(av);
    goto use_top;
  }

  /*
    If the size qualifies as a fastbin, first check corresponding bin.
  */
  if((chunk_size_t) nb <= (chunk_size_t) av->max_fast)
  {
    fb = &(av->fastbins[(fastbin_index(nb))]);
    if((victim = *fb) != 0)
    {
      *fb = victim->fd;
      check_remalloced_chunk(victim, nb);
      return chunk2mem(victim);
    }
  }

  /*
    If a small request, check regular bin. Since these "smallbins" hold one
    size each, no searching within bins is necessary. (For a large request, we
    need to wait until unsorted chunks are processed to find best fit. But for
    small ones, fits are exact anyway, so we can check now, which is faster.)
  */
  if(in_smallbin_range(nb))
  {
    idx = smallbin_index(nb);
    bin = bin_at(av,idx);

    if((victim = last(bin)) != bin)
    {
      bck = victim->bk;
      set_inuse_bit_at_offset(victim, nb);
      bin->bk = bck;
      bck->fd = bin;

      check_malloced_chunk(victim, nb);
      return chunk2mem(victim);
    }
  }
  /*
     If this is a large request, consolidate fastbins before continuing.  While
     it might look excessive to kill all fastbins before even seeing if there is
     space available, this avoids fragmentation problems normally associated
     with fastbins. Also, in practice, programs tend to have runs of either
     small or large requests, but less often mixtures, so consolidation is not
     invoked all that often in most programs. And the programs that it is called
     frequently in otherwise tend to fragment.
  */
  else
  {
    idx = largebin_index(nb);
    if(have_fastchunks(av))
      malloc_consolidate(av);
  }

  /*
    Process recently freed or remaindered chunks, taking one only if
    it is exact fit, or, if this a small request, the chunk is remainder from
    the most recent non-exact fit.  Place other traversed chunks in
    bins.  Note that this step is the only place in any routine where
    chunks are placed in bins.
  */
  while((victim = unsorted_chunks(av)->bk) != unsorted_chunks(av))
  {
    bck = victim->bk;
    size = chunksize(victim);

    /*
       If a small request, try to use last remainder if it is the
       only chunk in unsorted bin.  This helps promote locality for
       runs of consecutive small requests. This is the only
       exception to best-fit, and applies only when there is
       no exact fit for a small chunk.
    */
    if(in_smallbin_range(nb) && bck == unsorted_chunks(av) && victim == av->last_remainder && (chunk_size_t) size > (chunk_size_t) (nb + MINSIZE))
    {
      /* split and reattach remainder */
      remainder_size = size - nb;
      remainder = chunk_at_offset(victim, nb);
      unsorted_chunks(av)->bk = unsorted_chunks(av)->fd = remainder;
      av->last_remainder = remainder;
      remainder->bk = remainder->fd = unsorted_chunks(av);

      set_head(victim, nb | PREV_INUSE);
      set_head(remainder, remainder_size | PREV_INUSE);
      set_foot(remainder, remainder_size);

      check_malloced_chunk(victim, nb);
      return chunk2mem(victim);
    }

    /* remove from unsorted list */
    unsorted_chunks(av)->bk = bck;
    bck->fd = unsorted_chunks(av);

    /* Take now instead of binning if exact fit */
    if(size == nb)
    {
      set_inuse_bit_at_offset(victim, size);
      check_malloced_chunk(victim, nb);
      return chunk2mem(victim);
    }

    /* place chunk in bin */
    if(in_smallbin_range(size))
    {
      victim_index = smallbin_index(size);
      bck = bin_at(av, victim_index);
      fwd = bck->fd;
    }
    else
    {
      victim_index = largebin_index(size);
      bck = bin_at(av, victim_index);
      fwd = bck->fd;

      if(fwd != bck)
      {
        /* if smaller than smallest, place first */
        if((chunk_size_t)(size) < (chunk_size_t)(bck->bk->size))
        {
          fwd = bck;
          bck = bck->bk;
        }
        else if((chunk_size_t)(size) >= (chunk_size_t)(FIRST_SORTED_BIN_SIZE))
        {
          /* maintain large bins in sorted order */
          size |= PREV_INUSE; /* Or with inuse bit to speed comparisons */
          while((chunk_size_t) size < (chunk_size_t) fwd->size)
            fwd = fwd->fd;
          bck = fwd->bk;
        }
      }
    }

    mark_bin(av, victim_index);
    victim->bk = bck;
    victim->fd = fwd;
    fwd->bk = victim;
    bck->fd = victim;
  }

  /*
    If a large request, scan through the chunks of current bin to
    find one that fits.  (This will be the smallest that fits unless
    FIRST_SORTED_BIN_SIZE has been changed from default.)  This is
    the only step where an unbounded number of chunks might be
    scanned without doing anything useful with them. However the
    lists tend to be short.
  */
  if(!in_smallbin_range(nb))
  {
    bin = bin_at(av, idx);

    for(victim = last(bin); victim != bin; victim = victim->bk)
    {
      size = chunksize(victim);

      if((chunk_size_t) size >= (chunk_size_t) nb)
      {
        remainder_size = size - nb;
        unlink(victim, bck, fwd);

        /* Exhaust */
        if(remainder_size < MINSIZE)
        {
          set_inuse_bit_at_offset(victim, size);
          check_malloced_chunk(victim, nb);
          return chunk2mem(victim);
        }
        /* Split */
        remainder = chunk_at_offset(victim, nb);
        unsorted_chunks(av)->bk = unsorted_chunks(av)->fd = remainder;
        remainder->bk = remainder->fd = unsorted_chunks(av);
        set_head(victim, nb | PREV_INUSE);
        set_head(remainder, remainder_size | PREV_INUSE);
        set_foot(remainder, remainder_size);
        check_malloced_chunk(victim, nb);
        return chunk2mem(victim);
      }
    }
  }

  /*
    Search for a chunk by scanning bins, starting with next largest bin. This
    search is strictly by best-fit; i.e., the smallest (with ties going to
    approximately the least recently used) chunk that fits is selected.

    The bitmap avoids needing to check that most blocks are nonempty.
  */
  ++idx;
  bin = bin_at(av,idx);
  block = idx2block(idx);
  map = av->binmap[block];
  bit = idx2bit(idx);

  for(;;)
  {
    /* Skip rest of block if there are no more set bits in this block.  */
    if(bit > map || bit == 0)
    {
      do {
        if(++block >= BINMAPSIZE)  /* out of bins */
          goto use_top;
      } while((map = av->binmap[block]) == 0);

      bin = bin_at(av, (block << BINMAPSHIFT));
      bit = 1;
    }

    /* Advance to bin with set bit. There must be one. */
    while((bit & map) == 0)
    {
      bin = next_bin(bin);
      bit <<= 1;
      malloc_assert(bit != 0);
    }

    /* Inspect the bin. It is likely to be non-empty */
    victim = last(bin);

    /*  If a false alarm (empty bin), clear the bit. */
    if(victim == bin)
    {
      av->binmap[block] = map &= ~bit; /* Write through */
      bin = next_bin(bin);
      bit <<= 1;
    }
    else
    {
      size = chunksize(victim);

      /*  We know the first chunk in this bin is big enough to use. */
      malloc_assert((chunk_size_t) size >= (chunk_size_t) nb);

      remainder_size = size - nb;

      /* unlink */
      bck = victim->bk;
      bin->bk = bck;
      bck->fd = bin;

      /* Exhaust */
      if(remainder_size < MINSIZE)
      {
        set_inuse_bit_at_offset(victim, size);
        check_malloced_chunk(victim, nb);
        return chunk2mem(victim);
      }
      /* Split */
      remainder = chunk_at_offset(victim, nb);

      unsorted_chunks(av)->bk = unsorted_chunks(av)->fd = remainder;
      remainder->bk = remainder->fd = unsorted_chunks(av);
      /* advertise as last remainder */
      if(in_smallbin_range(nb))
        av->last_remainder = remainder;

      set_head(victim, nb | PREV_INUSE);
      set_head(remainder, remainder_size | PREV_INUSE);
      set_foot(remainder, remainder_size);
      check_malloced_chunk(victim, nb);
      return chunk2mem(victim);
    }
  }

  use_top:
  /*
    If large enough, split off the chunk bordering the end of memory
    (held in av->top). Note that this is in accord with the best-fit
    search rule.  In effect, av->top is treated as larger (and thus
    less well fitting) than any other available chunk since it can
    be extended to be as large as necessary (up to system
    limitations).

    We require that av->top always exists (i.e., has size >=
    MINSIZE) after initialization, so if it would otherwise be
    exhuasted by current request, it is replenished. (The main
    reason for ensuring it exists is that we may need MINSIZE space
    to put in fenceposts in sysmalloc.)
  */

  victim = av->top;
  size = chunksize(victim);

  if((chunk_size_t) size >= (chunk_size_t) (nb + MINSIZE))
  {
    remainder_size = size - nb;
    remainder = chunk_at_offset(victim, nb);
    av->top = remainder;
    set_head(victim, nb | PREV_INUSE);
    set_head(remainder, remainder_size | PREV_INUSE);

    check_malloced_chunk(victim, nb);
    return chunk2mem(victim);
  }

  /*
     If no space in top, relay to handle system-dependent cases
  */
  return sysmalloc(nb, av);
}


void free(void * mem)
{
  mstate av = get_malloc_state();

  mchunkptr       p;           /* chunk corresponding to mem */
  internal_size_t size;        /* its size */
  mfastbinptr*    fb;          /* associated fastbin */
  mchunkptr       nextchunk;   /* next contiguous chunk */
  internal_size_t nextsize;    /* its size */
  int             nextinuse;   /* true if nextchunk is used */
  internal_size_t prevsize;    /* size of previous contiguous chunk */
  mchunkptr       bck;         /* misc temp for linking */
  mchunkptr       fwd;         /* misc temp for linking */

  /* free(0) has no effect */
  if(mem)
  {
    p = mem2chunk(mem);
    size = chunksize(p);

    check_inuse_chunk(p);

    /*
      If eligible, place chunk on a fastbin so it can be found
      and used quickly in malloc.
    */

    if((chunk_size_t) size <= (chunk_size_t) av->max_fast
#if TRIM_FASTBINS
        /*
           If TRIM_FASTBINS set, don't place chunks
           bordering top into fastbins
        */
        && (chunk_at_offset(p, size) != av->top)
#endif
        )
    {
      set_fastchunks(av);
      fb = &(av->fastbins[fastbin_index(size)]);
      p->fd = *fb;
      *fb = p;
    }
    /*
       Consolidate other chunks as they arrive.
    */
    else
    {
      set_anychunks(av);

      nextchunk = chunk_at_offset(p, size);
      nextsize = chunksize(nextchunk);

      /* consolidate backward */
      if(!prev_inuse(p))
      {
        prevsize = p->prev_size;
        size += prevsize;
        p = chunk_at_offset(p, -((long) prevsize));
        unlink(p, bck, fwd);
      }

      if(nextchunk != av->top)
      {
        /* get and clear inuse bit */
        nextinuse = inuse_bit_at_offset(nextchunk, nextsize);
        set_head(nextchunk, nextsize);

        /* consolidate forward */
        if(!nextinuse)
        {
          unlink(nextchunk, bck, fwd);
          size += nextsize;
        }

        /*
          Place the chunk in unsorted chunk list. Chunks are
          not placed into regular bins until after they have
          been given one chance to be used in malloc.
        */

        bck = unsorted_chunks(av);
        fwd = bck->fd;
        p->bk = bck;
        p->fd = fwd;
        bck->fd = p;
        fwd->bk = p;

        set_head(p, size | PREV_INUSE);
        set_foot(p, size);

        check_free_chunk(p);
      }
      /*
         If the chunk borders the current high end of memory,
         consolidate into top
      */
      else
      {
        size += nextsize;
        set_head(p, size | PREV_INUSE);
        av->top = p;
        check_chunk(p);
      }

      /*
        If freeing a large space, consolidate possibly-surrounding
        chunks. Then, if the total unused topmost memory exceeds trim
        threshold, ask malloc_trim to reduce top.

        Unless max_fast is 0, we don't know if there are fastbins
        bordering top, so we cannot tell for sure whether threshold
        has been reached unless fastbins are consolidated.  But we
        don't want to consolidate on each free.  As a compromise,
        consolidation is performed if FASTBIN_CONSOLIDATION_THRESHOLD
        is reached.
      */

      if((chunk_size_t) size >= FASTBIN_CONSOLIDATION_THRESHOLD)
      {
        if(have_fastchunks(av))
          malloc_consolidate(av);

        if((chunk_size_t) chunksize(av->top) >= (chunk_size_t) av->trim_threshold)
          systrim(av->top_pad, av);
      }
    }
  }
}

/*
  ------------------------- malloc_consolidate -------------------------

  malloc_consolidate is a specialized version of free() that tears
  down chunks held in fastbins.  Free itself cannot be used for this
  purpose since, among other things, it might place chunks back onto
  fastbins.  So, instead, we need to use a minor variant of the same
  code.

  Also, because this routine needs to be called the first time through
  malloc anyway, it turns out to be the perfect place to trigger
  initialization code.
*/

static void malloc_consolidate(mstate av)
{
  mfastbinptr *   fb;                 /* current fastbin being consolidated */
  mfastbinptr *   maxfb;              /* last fastbin (for loop control) */
  mchunkptr       p;                  /* current chunk being consolidated */
  mchunkptr       nextp;              /* next chunk to consolidate */
  mchunkptr       unsorted_bin;       /* bin header */
  mchunkptr       first_unsorted;     /* chunk to link to */

  /* These have same use as in free() */
  mchunkptr       nextchunk;
  internal_size_t size;
  internal_size_t nextsize;
  internal_size_t prevsize;
  int             nextinuse;
  mchunkptr       bck;
  mchunkptr       fwd;

  /*
    If max_fast is 0, we know that av hasn't
    yet been initialized, in which case do so below
  */
  if(av->max_fast != 0)
  {
    clear_fastchunks(av);

    unsorted_bin = unsorted_chunks(av);

    /*
      Remove each chunk from fast bin and consolidate it, placing it
      then in unsorted bin. Among other reasons for doing this,
      placing in unsorted bin avoids needing to calculate actual bins
      until malloc is sure that chunks aren't immediately going to be
      reused anyway.
    */

    maxfb = &(av->fastbins[fastbin_index(av->max_fast)]);
    fb = &(av->fastbins[0]);
    do {
      if((p = *fb) != 0)
      {
        *fb = 0;

        do {
          check_inuse_chunk(p);
          nextp = p->fd;

          /* Slightly streamlined version of consolidation code in free() */
          size = p->size & ~PREV_INUSE;
          nextchunk = chunk_at_offset(p, size);
          nextsize = chunksize(nextchunk);

          if(!prev_inuse(p))
          {
            prevsize = p->prev_size;
            size += prevsize;
            p = chunk_at_offset(p, -((long) prevsize));
            unlink(p, bck, fwd);
          }

          if(nextchunk != av->top)
          {
            nextinuse = inuse_bit_at_offset(nextchunk, nextsize);
            set_head(nextchunk, nextsize);

            if(!nextinuse)
            {
              size += nextsize;
              unlink(nextchunk, bck, fwd);
            }

            first_unsorted = unsorted_bin->fd;
            unsorted_bin->fd = p;
            first_unsorted->bk = p;

            set_head(p, size | PREV_INUSE);
            p->bk = unsorted_bin;
            p->fd = first_unsorted;
            set_foot(p, size);
          }
          else
          {
            size += nextsize;
            set_head(p, size | PREV_INUSE);
            av->top = p;
          }
        } while((p = nextp) != 0);
      }
    } while(fb++ != maxfb);
  }
  else
  {
    malloc_init_state(av);
    check_malloc_state();
  }
}


void * realloc(void * oldmem, size_t bytes)
{
  mstate av = get_malloc_state();

  internal_size_t  nb;              /* padded request size */

  mchunkptr        oldp;            /* chunk corresponding to oldmem */
  internal_size_t  oldsize;         /* its size */

  mchunkptr        newp;            /* chunk to return */
  internal_size_t  newsize;         /* its size */
  void*          newmem;          /* corresponding user mem */

  mchunkptr        next;            /* next contiguous chunk after oldp */

  mchunkptr        remainder;       /* extra space at end of newp */
  chunk_size_t     remainder_size;  /* its size */

  mchunkptr        bck;             /* misc temp for linking */
  mchunkptr        fwd;             /* misc temp for linking */

  chunk_size_t     copysize;        /* bytes to copy */
  unsigned int     ncopies;         /* internal_size_t words to copy */
  internal_size_t* s;               /* copy source */
  internal_size_t* d;               /* copy destination */


#ifdef REALLOC_ZERO_BYTES_FREES
  if(bytes == 0)
  {
    free(oldmem);
    return 0;
  }
#endif

  /* realloc of null is supposed to be same as malloc */
  if(oldmem == 0)
    return malloc(bytes);

  checked_request2size(bytes, nb);

  oldp    = mem2chunk(oldmem);
  oldsize = chunksize(oldp);

  check_inuse_chunk(oldp);

  if((chunk_size_t) oldsize >= (chunk_size_t) nb)
  {
    /* already big enough; split below */
    newp = oldp;
    newsize = oldsize;
  }
  else
  {
    next = chunk_at_offset(oldp, oldsize);

    /* Try to expand forward into top */
    if(next == av->top && (chunk_size_t) (newsize = oldsize + chunksize(next)) >= (chunk_size_t) (nb + MINSIZE))
    {
      set_head_size(oldp, nb);
      av->top = chunk_at_offset(oldp, nb);
      set_head(av->top, (newsize - nb) | PREV_INUSE);
      return chunk2mem(oldp);
    }
    /* Try to expand forward into next chunk;  split off remainder below */
    else if(next != av->top && !inuse(next) && (chunk_size_t) (newsize = oldsize + chunksize(next)) >= (chunk_size_t) nb)
    {
      newp = oldp;
      unlink(next, bck, fwd);
    }
    /* allocate, copy, free */
    else
    {
      newmem = malloc(nb - MALLOC_ALIGN_MASK);
      if(newmem == 0)
        return 0; /* propagate failure */

      newp = mem2chunk(newmem);
      newsize = chunksize(newp);

      /*
        Avoid copy if newp is next chunk after oldp.
      */
      if(newp == next)
      {
        newsize += oldsize;
        newp = oldp;
      }
      else
      {
        /*
          Unroll copy of <= 36 bytes (72 if 8byte sizes)
          We know that contents have an odd number of
          internal_size_t-sized words; minimally 3.
        */

        copysize = oldsize - SIZE_SZ;
        s = (internal_size_t *) oldmem;
        d = (internal_size_t *) newmem;
        ncopies = copysize / sizeof(internal_size_t);
        malloc_assert(ncopies >= 3);

        if(ncopies > 9)
          MALLOC_COPY(d, s, copysize);
        else
        {
          *(d + 0) = *(s + 0);
          *(d + 1) = *(s + 1);
          *(d + 2) = *(s + 2);
          if(ncopies > 4)
          {
            *(d + 3) = *(s + 3);
            *(d + 4) = *(s + 4);
            if(ncopies > 6)
            {
              *(d + 5) = *(s + 5);
              *(d + 6) = *(s + 6);
              if(ncopies > 8)
              {
                *(d + 7) = *(s + 7);
                *(d + 8) = *(s + 8);
              }
            }
          }
        }

        free(oldmem);
        check_inuse_chunk(newp);
        return chunk2mem(newp);
      }
    }
  }

  /* If possible, free extra space in old or extended chunk */

  malloc_assert((chunk_size_t)(newsize) >= (chunk_size_t)(nb));

  remainder_size = newsize - nb;

  if(remainder_size < MINSIZE)
  {
    /* not enough extra to split off */
    set_head_size(newp, newsize);
    set_inuse_bit_at_offset(newp, newsize);
  }
  else
  {
    /* split remainder */
    remainder = chunk_at_offset(newp, nb);
    set_head_size(newp, nb);
    set_head(remainder, remainder_size | PREV_INUSE);
    /* Mark remainder as inuse so free() won't complain */
    set_inuse_bit_at_offset(remainder, remainder_size);
    free(chunk2mem(remainder));
  }

  check_inuse_chunk(newp);
  return chunk2mem(newp);
}


void * memalign(size_t alignment, size_t bytes)
{
  internal_size_t nb;             /* padded  request size */
  char *          m;              /* memory returned by malloc call */
  mchunkptr       p;              /* corresponding chunk */
  char *          brk;            /* alignment point within p */
  mchunkptr       newp;           /* chunk to return */
  internal_size_t newsize;        /* its size */
  internal_size_t leadsize;       /* leading space before alignment point */
  mchunkptr       remainder;      /* spare room at end to split off */
  chunk_size_t    remainder_size; /* its size */
  internal_size_t size;

  /* If need less alignment than we give anyway, just relay to malloc */

  if(alignment <= MALLOC_ALIGNMENT)
    return malloc(bytes);

  /* Otherwise, ensure that it is at least a minimum chunk size */

  if(alignment <  MINSIZE)
    alignment = MINSIZE;

  /* Make sure alignment is power of 2 (in case MINSIZE is not).  */
  if((alignment & (alignment - 1)) != 0)
  {
    size_t a = MALLOC_ALIGNMENT * 2;
    while((chunk_size_t) a < (chunk_size_t) alignment)
      a <<= 1;
    alignment = a;
  }

  checked_request2size(bytes, nb);

  /*
    Strategy: find a spot within that chunk that meets the alignment
    request, and then possibly free the leading and trailing space.
  */

  /* Call malloc with worst case padding to hit alignment. */
  m  = (char *) malloc(nb + alignment + MINSIZE);

  /* propagate failure */
  if(m == 0)
    return 0;

  p = mem2chunk(m);

  if((((uintptr_t)(m)) % alignment) != 0)
  {
    /*
      Find an aligned spot inside chunk.  Since we need to give back
      leading space in a chunk of at least MINSIZE, if the first
      calculation places us at a spot with less than MINSIZE leader,
      we can move to the next aligned spot -- we've allocated enough
      total room so that this is always possible.
    */

    brk = (char *) mem2chunk((uintptr_t) (((uintptr_t) (m + alignment - 1)) & -((signed long) alignment)));
    if((chunk_size_t) (brk - (char *) (p)) < MINSIZE)
      brk += alignment;

    newp = (mchunkptr) brk;
    leadsize = brk - (char *) p;
    newsize = chunksize(p) - leadsize;

    /* give back leader, use the rest */
    set_head(newp, newsize | PREV_INUSE);
    set_inuse_bit_at_offset(newp, newsize);
    set_head_size(p, leadsize);
    free(chunk2mem(p));
    p = newp;

    malloc_assert(newsize >= nb && (((uintptr_t) (chunk2mem(p))) % alignment) == 0);
  }

  /* Also give back spare room at the end */
  size = chunksize(p);
  if((chunk_size_t) size > (chunk_size_t) (nb + MINSIZE))
  {
    remainder_size = size - nb;
    remainder = chunk_at_offset(p, nb);
    set_head(remainder, remainder_size | PREV_INUSE);
    set_head_size(p, nb);
    free(chunk2mem(remainder));
  }

  check_inuse_chunk(p);
  return chunk2mem(p);
}


void * calloc(size_t n_elements, size_t elem_size)
{
  mchunkptr p;
  chunk_size_t  clearsize;
  chunk_size_t  nclears;
  internal_size_t * d;

  void * mem = malloc(n_elements * elem_size);

  if(mem)
  {
    p = mem2chunk(mem);

    /*
      Unroll clear of <= 36 bytes (72 if 8byte sizes)
      We know that contents have an odd number of
      internal_size_t-sized words; minimally 3.
    */
    d = (internal_size_t *) mem;
    clearsize = chunksize(p) - SIZE_SZ;
    nclears = clearsize / sizeof(internal_size_t);
    malloc_assert(nclears >= 3);

    if(nclears > 9)
      MALLOC_ZERO(d, clearsize);
    else
    {
      *(d + 0) = 0;
      *(d + 1) = 0;
      *(d + 2) = 0;
      if(nclears > 4)
      {
        *(d + 3) = 0;
        *(d + 4) = 0;
        if(nclears > 6)
        {
          *(d + 5) = 0;
          *(d + 6) = 0;
          if(nclears > 8)
          {
            *(d + 7) = 0;
            *(d + 8) = 0;
          }
        }
      }
    }
  }
  return mem;
}


void ** independent_calloc(size_t n_elements, size_t elem_size, void * chunks[])
{
  size_t sz = elem_size; /* serves as 1-element array */
  /* opts arg of 3 means all elements are same size, and should be cleared */
  return independent_alloc(n_elements, &sz, 3, chunks);
}


void ** independent_comalloc(size_t n_elements, size_t sizes[], void * chunks[])
{
  return independent_alloc(n_elements, sizes, 0, chunks);
}


/*
  ------------------------- independent_alloc -----------------------
  independent_alloc provides common support for independent_X routines,
  handling all of the combinations that can result.

  The opts arg has:
    bit 0 set if all elements are same size (using sizes[0])
    bit 1 set if elements should be zeroed
*/

static void ** independent_alloc(size_t n_elements, size_t * sizes, int opts, void * chunks[])
{
  mstate av = get_malloc_state();
  internal_size_t element_size;   /* chunksize of each element, if all same */
  internal_size_t contents_size;  /* total size of elements */
  internal_size_t array_size;     /* request size of pointer array */
  void *          mem;            /* malloced aggregate space */
  mchunkptr       p;              /* corresponding chunk */
  internal_size_t remainder_size; /* remaining bytes while splitting */
  void **         marray;         /* either "chunks" or malloced ptr array */
  mchunkptr       array_chunk;    /* chunk for malloced ptr array */
  internal_size_t size;
  size_t          i;

  /* Ensure initialization */
  if(av->max_fast == 0)
    malloc_consolidate(av);

  /* compute array length, if needed */
  if(chunks != 0)
  {
    if(n_elements == 0)
      return chunks; /* nothing to do */
    marray = chunks;
    array_size = 0;
  }
  else
  {
    /* if empty req, must still return chunk representing empty array */
    if(n_elements == 0)
      return (void **) malloc(0);
    marray = 0;
    array_size = request2size(n_elements * (sizeof(void *)));
  }

  /* compute total element size */
  if(opts & 0x1)
  {
    /* all-same-size */
    element_size = request2size(*sizes);
    contents_size = n_elements * element_size;
  }
  else
  {
    /* add up all the sizes */
    element_size = 0;
    contents_size = 0;
    for(i = 0; i != n_elements; ++i)
      contents_size += request2size(sizes[i]);
  }

  /* subtract out alignment bytes from total to minimize overallocation */
  size = contents_size + array_size - MALLOC_ALIGN_MASK;

  mem = malloc(size);
  if(!mem)
    return 0;

  p = mem2chunk(mem);
  remainder_size = chunksize(p);

  /* optionally clear the elements */
  if(opts & 0x2)
    MALLOC_ZERO(mem, remainder_size - SIZE_SZ - array_size);

  /* If not provided, allocate the pointer array as final part of chunk */
  if(marray == 0)
  {
    array_chunk = chunk_at_offset(p, contents_size);
    marray = (void **) (chunk2mem(array_chunk));
    set_head(array_chunk, (remainder_size - contents_size) | PREV_INUSE);
    remainder_size = contents_size;
  }

  /* split out elements */
  for(i = 0; ; ++i)
  {
    marray[i] = chunk2mem(p);
    if(i != n_elements-1)
    {
      if(element_size != 0)
        size = element_size;
      else
        size = request2size(sizes[i]);
      remainder_size -= size;
      set_head(p, size | PREV_INUSE);
      p = chunk_at_offset(p, size);
    }
    else
    {
      /* the final element absorbs any overallocation slop */
      set_head(p, remainder_size | PREV_INUSE);
      break;
    }
  }

#if DEBUG
  if(marray != chunks)
  {
    /* final element must have exactly exhausted chunk */
    if(element_size != 0)
      malloc_assert(remainder_size == element_size);
    else
      malloc_assert(remainder_size == request2size(sizes[i]));
    check_inuse_chunk(mem2chunk(marray));
  }

  for(i = 0; i != n_elements; ++i)
    check_inuse_chunk(mem2chunk(marray[i]));
#endif

  return marray;
}


void * valloc(size_t bytes)
{
  /* Ensure initialization */
  mstate av = get_malloc_state();
  if(av->max_fast == 0)
    malloc_consolidate(av);
  return memalign(av->pagesize, bytes);
}


void * pvalloc(size_t bytes)
{
  mstate av = get_malloc_state();
  size_t pagesz;

  /* Ensure initialization */
  if(av->max_fast == 0)
    malloc_consolidate(av);
  pagesz = av->pagesize;
  return memalign(pagesz, (bytes + pagesz - 1) & ~(pagesz - 1));
}


int malloc_trim(size_t pad)
{
  mstate av = get_malloc_state();
  /* Ensure initialization/consolidation */
  malloc_consolidate(av);

  return systrim(pad, av);
}


size_t malloc_usable_size(void * mem)
{
  mchunkptr p;
  if(mem)
  {
    p = mem2chunk(mem);
    if(inuse(p))
      return chunksize(p) - SIZE_SZ;
  }
  return 0;
}


struct mallinfo mallinfo()
{
  mstate av = get_malloc_state();
  struct mallinfo mi;
  int i;
  mbinptr b;
  mchunkptr p;
  internal_size_t avail;
  internal_size_t fastavail;
  int nblocks;
  int nfastblocks;

  /* Ensure initialization */
  if(av->top == 0)
    malloc_consolidate(av);

  check_malloc_state();

  /* Account for top */
  avail = chunksize(av->top);
  nblocks = 1;  /* top always exists */

  /* traverse fastbins */
  nfastblocks = 0;
  fastavail = 0;

  for(i = 0; i < NFASTBINS; ++i)
    for(p = av->fastbins[i]; p != 0; p = p->fd)
    {
      ++nfastblocks;
      fastavail += chunksize(p);
    }

  avail += fastavail;

  /* traverse regular bins */
  for(i = 1; i < NBINS; ++i)
  {
    b = bin_at(av, i);
    for(p = last(b); p != b; p = p->bk)
    {
      ++nblocks;
      avail += chunksize(p);
    }
  }

  mi.smblks = nfastblocks;
  mi.ordblks = nblocks;
  mi.fordblks = avail;
  mi.uordblks = av->sbrked_mem - avail;
  mi.arena = av->sbrked_mem;
  mi.fsmblks = fastavail;
  mi.keepcost = chunksize(av->top);
  mi.usmblks = av->max_total_mem;
  return mi;
}


void malloc_stats()
{
  struct mallinfo mi = mallinfo();

  printf("max system bytes = %10lu\n", (chunk_size_t) (mi.usmblks));
  printf("system bytes     = %10lu\n", (chunk_size_t) (mi.arena));
  printf("in use bytes     = %10lu\n", (chunk_size_t) (mi.uordblks));
}


int mallopt(int param_number, int value)
{
  mstate av = get_malloc_state();
  /* Ensure initialization/consolidation */
  malloc_consolidate(av);

  switch(param_number)
  {
    case M_MXFAST:
      if(value >= 0 && value <= MAX_FAST_SIZE)
      {
        set_max_fast(av, value);
        return 1;
      }
      return 0;

    case M_TRIM_THRESHOLD:
      av->trim_threshold = value;
      return 1;

    case M_TOP_PAD:
      av->top_pad = value;
      return 1;

    default:
      return 0;
  }
}

/* sbrk() for KudOS */

/* up to 256M memory */
#define SBRK_MEM_START 0x10000000
#define SBRK_MEM_STOP 0x20000000
#define SBRK_MEM_SIZE (SBRK_MEM_STOP - SBRK_MEM_START)
static uint32_t sbrk_size = 0;
static uint32_t vmem_pages = 0;

static void * sbrk(size_t incr)
{
	if(incr < 0)
	{
		uint32_t new_pages;
		incr = -incr;
		if(incr > sbrk_size)
			return (void *) MORECORE_FAILURE;
		sbrk_size -= incr;
		new_pages = (sbrk_size + PGSIZE - 1) / PGSIZE;
		while(vmem_pages > new_pages)
			sys_page_unmap(0, (void *) (SBRK_MEM_START + PGSIZE * --vmem_pages));
	}
	else if(incr)
	{
		uint32_t new_pages;
		uint32_t begin = sbrk_size;
		if(sbrk_size + incr > SBRK_MEM_STOP)
			return (void *) MORECORE_FAILURE;
		sbrk_size += incr;
		new_pages = (sbrk_size + PGSIZE - 1) / PGSIZE;
		while(vmem_pages < new_pages)
			if(sys_page_alloc(0, (void *) (SBRK_MEM_START + PGSIZE * vmem_pages++), PTE_U | PTE_W | PTE_P))
			{
				vmem_pages--;
				sbrk_size = begin;
				new_pages = (sbrk_size + PGSIZE - 1) / PGSIZE;
				while(vmem_pages > new_pages)
					sys_page_unmap(0, (void *) (SBRK_MEM_START + PGSIZE * --vmem_pages));
				return (void *) MORECORE_FAILURE;
			}
		return (void *) (SBRK_MEM_START + begin);
	}
	/* sbrk(<=0) */
	return (void *) (SBRK_MEM_START + sbrk_size);
}

#endif /* ] USE_FAILFAST_MALLOC */
