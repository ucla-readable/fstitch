/* See COPYRIGHT for copyright information. */

#ifndef _CONSOLE_H_
#define _CONSOLE_H_
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>
#include <inc/env.h>
#include <inc/serial.h>

#define MONO_BASE 0x3b4
#define MONO_BUF 0xb0000
#define CGA_BASE 0x3d4
#define CGA_BUF 0xb8000

#define CRT_ROWS 25
#define CRT_COLS 80
#define CRT_SIZE (CRT_ROWS * CRT_COLS)

struct Com {
	envid_t   user;
	uint16_t  addr; // 0 means this Com not present
	uintptr_t buf;
};

extern struct Com com[NCOMS];

envid_t Com_user(int port);

void cons_init(void);
void cons_putc(int c);
int cons_getc(void);

/* needed by mouse_intr() */
void kbd_intr(int irq);

#endif /* _CONSOLE_H_ */
