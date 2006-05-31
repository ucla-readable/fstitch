#ifndef KUDOS_KERN_PCSPK_H
#define KUDOS_KERN_PCSPK_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

int pcspk_init(void);

/* system calls */
int pcspk_close(void);
int pcspk_open(uint16_t rate, uint8_t output, uintptr_t address);
int pcspk_setvolume(uint8_t volume);
int pcspk_start(void);
int pcspk_stop(void);
int pcspk_wait(void);

extern int sb_use_pcspk;

#endif /* !KUDOS_KERN_PCSPK_H */
