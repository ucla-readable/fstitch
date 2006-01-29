#ifndef KUDOS_INC_STDLIB_H
#define KUDOS_INC_STDLIB_H

#include <inc/types.h>
#include <inc/malloc.h>

void exit(int status);

// Sort in ascending order. compar should return a value less than,
// equal to, or greater than zero if 'a' is less than, equal to, or
// greater than 'b', respectively.
void qsort(void * base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

#endif /* !KUDOS_INC_STDLIB_H */
