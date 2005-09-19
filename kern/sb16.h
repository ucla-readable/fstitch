#ifndef KUDOS_KERN_SB16_H
#define KUDOS_KERN_SB16_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

#define SB16_PORT 0x220
#define SB16_IRQ 5
#define SB16_DMA 1
#define SB16_DMA16 5

void sb16_init(void);

/* system calls */
int sb16_close(void);
int sb16_open(uint16_t rate, uint8_t output, uintptr_t address);
int sb16_setvolume(uint8_t volume);
int sb16_start(void);
int sb16_stop(void);
int sb16_wait(void);

#endif /* !KUDOS_KERN_SB16_H */
