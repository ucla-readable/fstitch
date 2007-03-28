#include <lib/platform.h>
#include <lib/svnrevtol.h>

// Returns the revision number specified in the given subversion rev string.
long
svnrevtol(const char *rev_str)
{
	// ignore the "$ Rev:" before the rev number
	// (strtol will ignore the " $" that is after the rev number)
	rev_str = &rev_str[6]; 

	return strtol(rev_str, NULL, 10);
}
