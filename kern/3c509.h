#ifndef KUDOS_KERN_3C509_H
#define KUDOS_KERN_3C509_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

#define EL3_FLAG_PROMISC 0x1
#define EL3_FLAG_MULTICAST 0x2

int el3_init(void);

/* called from syscall */
int el3_allocate(int which);
int el3_release(int which);
int el3_get_address(int which, uint8_t * buffer);
int el3_set_filter(int which, int flags);
int el3_tx_reset(int which);
int el3_send_packet(int which, const void * data, int length);
int el3_query(int which);
int el3_get_packet(int which, void * data, int length);

#endif /* !KUDOS_KERN_3C509_H */
