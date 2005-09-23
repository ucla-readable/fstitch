#ifndef KUDOS_INC_IRQ_H
#define KUDOS_INC_IRQ_H
#ifdef KUDOS_KERNEL
# error "This is a KudOS user library header; the kernel should not #include it"
#endif

#define MAX_IRQS 16

typedef void (*irq_handler_t)(int irq);

int request_irq(int irq, irq_handler_t handler);

#endif /* !KUDOS_INC_IRQ_H */
