#ifndef KUDOS_INC_SERIAL_H
#define KUDOS_INC_SERIAL_H

#include <inc/types.h>
#include <inc/mmu.h>

#define NCOMS 4

//
// Recv buffer

// a buffer is SBUFSIZE bytes, a buf_begin idx, and a buf_end idx

#define SBUFSIZE (PGSIZE - 2*sizeof(uint16_t))

uint16_t get_buf_begin(uint8_t *buf);
uint16_t inc_buf_begin(uint8_t *buf);
uint16_t get_buf_end(uint8_t *buf);
uint16_t inc_buf_end(uint8_t *buf);

static __inline uint16_t get_buf_free(uint16_t begin_idx, uint16_t end_idx) __attribute__((always_inline));
static __inline uint16_t
get_buf_free(uint16_t begin_idx, uint16_t end_idx)
{
	return (SBUFSIZE - end_idx + begin_idx - 1) % SBUFSIZE;
}

static __inline uint16_t get_buf_avail(uint16_t begin_idx, uint16_t end_idx) __attribute__((always_inline));
static __inline uint16_t
get_buf_avail(uint16_t begin_idx, uint16_t end_idx)
{
	return SBUFSIZE - get_buf_free(begin_idx, end_idx) - 1;
}

//
// Sending

void     serial_sendc(uint8_t c, uint16_t ioaddr);

#endif // KUDOS_INC_SERIAL_H
