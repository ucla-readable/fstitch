/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

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
