// KudOS serial io for lwip, used for SLIP or PPP.

#include "arch/sys_arch.h"
#include "lwip/sio.h"
#include <inc/mmu.h>
#include <inc/lib.h>
#include <inc/serial.h>
#include <inc/x86.h>
#include <lib/types.h>

#define SIO_FROST_DEBUG 0

// TODO: how do I set the iface configs, such as speed, parity, ...?

#define NSIODEVS NCOMS
struct sio_dev siodevs[NSIODEVS];

sio_fd_t
sio_open(u8_t devnum)
{
	if(devnum > NSIODEVS)
		panic("Tried to use devnum %d but only %d sio devs are allowed\n",
				devnum, NSIODEVS);

	sio_fd_t fd = &siodevs[devnum];
	fd->sioread = 0;
	fd->buf     = (uint8_t*) ROUND32(fd->buf_container, PGSIZE);

	int r;
	
	// HACK: mark buf as shared so that fork() won't make the page COW.
	const int perm = PTE_P|PTE_U|PTE_W|PTE_SHARE;
	if(! (get_pte(fd->buf) & PTE_SHARE) )
	{
		fd->buf[0] = 0; // Ensure the page is not PTE_COW
		if((r = sys_page_map(0, fd->buf, 0, fd->buf, perm)) < 0)
			panic("sys_page_map: %e", r);
	}
	else
	{
		// The current env was started by another env also using this
		// code, unmap buf so that it isn't shared with the other env.

		if((r = sys_page_unmap(0, fd->buf)) < 0)
			panic("sys_page_unmap: %e", r);
		if((r = sys_page_alloc(0, fd->buf, perm)) < 0)
			panic("sys_page_alloc: %e", r);
	}


	if((r = sys_reg_serial(-1, fd->buf)) < 0)
		return NULL;

	fd->com_addr = r;

	return fd;
}

u8_t
sio_recv(sio_fd_t fd)
{
	u8_t x;
	
	uint16_t buf_begin, buf_end;
	buf_begin = get_buf_begin(fd->buf);
	buf_end   = get_buf_end(fd->buf);

	if(get_buf_avail(buf_begin, buf_end) == 0)
	{
		fd->sioread = 0;
		return 0;
	}
	else
	{
#if SIO_FROST_DEBUG
		kdprintf(1, "#");
#endif
		fd->sioread = 1;
	}

	x = fd->buf[buf_begin];
	fd->buf[buf_begin] = 0;
	inc_buf_begin(fd->buf);

	return x;
}

void
sio_send(u8_t c, sio_fd_t fd)
{
#if SIO_FROST_DEBUG
	kdprintf(1, "@");
#endif
	serial_sendc(c, fd->com_addr);
}

#if 0
// Not needed by slip, used by ppp.

u32_t
sio_read(sio_fd_t fd, u8_t *pc, u32_t len)
{
	panic("sio_open: implement");
	u32_t x;
	return x;
}

u32_t
sio_write(sio_fd_t fd, u8_t *pc, u32_t len)
{
	panic("sio_open: implement");
	u32_t x;
	return x;
}

void
sio_read_abort(sio_fd_t fd)
{
	panic("sio_open: implement");
}

#endif
