#ifndef KUDOS_KERN_3C509_H
#define KUDOS_KERN_3C509_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

#define EL3_FLAG_PROMISC 0x1
#define EL3_FLAG_MULTICAST 0x2

int el3_init(void);

#endif /* !KUDOS_KERN_3C509_H */
