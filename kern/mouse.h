#ifndef KUDOS_KERN_MOUSE_H
#define KUDOS_KERN_MOUSE_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

#define MOUSE_IRQ 12

int mouse_detect(void);
int mouse_read(uint8_t * buffer, int size);
int mouse_command(uint8_t command);
int mouse_init(void);

/* needed by kbd_intr() */
void mouse_intr(int irq);

#endif /* !KUDOS_KERN_MOUSE_H */
