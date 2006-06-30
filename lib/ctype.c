#include <inc/ctype.h>

int
isupper(int c)
{
	return (c >= 'A' && c <= 'Z');
}

int
islower(int c)
{
	return (c >= 'a' && c <= 'z');
}

int
toupper(int c)
{
	if (islower(c))
		return c + 'A' - 'a';
	else
		return c;
}

int
tolower(int c)
{
	if (isupper(c))
		return c - 'A' + 'a';
	else
		return c;
}
