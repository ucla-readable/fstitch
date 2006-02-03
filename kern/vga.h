#ifndef KUDOS_KERN_VGA_H
#define KUDOS_KERN_VGA_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

#define VGA_PMEM (0xA0000)
#define VGA_MEM ((uint8_t *) KADDR(VGA_PMEM))
#define VGA_MEM_SIZE 0x10000

void vga_save_palette(uint8_t * buffer);
void vga_set_palette(const uint8_t * buffer, uint8_t dim);

int vga_set_mode_320(int fade);
int vga_set_mode_text(int fade);

#endif /* !KUDOS_KERN_VGA_H */
