#ifndef KUDOS_KERN_IRQ_H
#define KUDOS_KERN_IRQ_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/env.h>
#include <inc/trap.h>

typedef void (*irq_handler_t)(int irq);

int request_irq(int irq, irq_handler_t handler);
void dispatch_irq(int irq);
void probe_irq_on(void);
int probe_irq_off(void);

int env_dispatch_irqs(void);

int env_assign_irq(int irq, struct Env * env);
int env_unassign_irq(int irq, struct Env * env);
int env_irq_unassign_all(struct Env * env);

#endif /* KUDOS_KERN_IRQ_H */
