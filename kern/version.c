#include <inc/stdio.h>
#include <inc/config.h>

void
version(void)
{
	printf("KudOS kernel (" RELEASE_NAME ") compiled at " __TIME__ " on " __DATE__ "\n");
}
