#include <inc/lib.h>

void
umain(int argc, char **argv)
{
        printf("%d\n", hwclock_time(NULL));
}
