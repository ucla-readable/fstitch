// Eat chunks spewed by others.

#include <inc/lib.h>

void
umain()
{
	uint8_t buffer[1024];

	while(read(STDIN_FILENO, buffer, sizeof(buffer)));
}
