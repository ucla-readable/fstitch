#ifndef KUDOS_KERN_NE_H
#define KUDOS_KERN_NE_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

int ne_init(void);

#endif /* !KUDOS_KERN_NE_H */
