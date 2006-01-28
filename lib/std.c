#if defined(KUDOS)
#include <lib.h>
#elif defined(UNIXUSER)
#include <unistd.h>
#else
#error Unknown target
#endif

#include <lib/std.h>

ssize_t
readn(int fdnum, void* buf, size_t n)
{
	ssize_t m, tot;

	for (tot = 0; tot < n; tot += m) {
		m = read(fdnum, ((char*) buf) + tot, n - tot);
		if (m < 0)
			return m;
		if (m == 0)
			break;
	}
	return tot;
}
