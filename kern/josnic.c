#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/error.h>
#include <inc/string.h>

#include <kern/kclock.h>
#include <kern/josnic.h>

#define MAX_JOSNIC_DEVS 8

static struct {
	const struct josnic * nic;
	uint16_t valid:1, enabled: 1;
	int trans_start, last_rx, drv_which;
} josnic_dev[MAX_JOSNIC_DEVS] = {{valid: 0}};
static int josnic_devs = 0;

#define MAX_BUFFER_PACKETS 128
#define PACKET_BUFFER_SIZE 8192

static struct {
	uint16_t pkt_free, pkt_ready;
	struct { uint16_t offset, length; } pkt[MAX_BUFFER_PACKETS];
	uint16_t pb_free, pb_ready;
	/* rather than worry about wrapping packets that cross the border, just fudge the size */
	uint8_t packet_buffer[PACKET_BUFFER_SIZE + 1536];
} josnic_pkb[MAX_JOSNIC_DEVS];

#define pkt_free josnic_pkb[which].pkt_free
#define pkt_ready josnic_pkb[which].pkt_ready
#define josnic_pkt josnic_pkb[which].pkt
#define pb_free josnic_pkb[which].pb_free
#define pb_ready josnic_pkb[which].pb_ready
#define packet_buffer josnic_pkb[which].packet_buffer

#define READY_PACKETS ((pkt_free - pkt_ready + MAX_BUFFER_PACKETS) % MAX_BUFFER_PACKETS)
#define READY_BUFFER ((pb_free - pb_ready + PACKET_BUFFER_SIZE) % PACKET_BUFFER_SIZE)

/* subtract 1 to keep the buffer from filling entirely and looking empty again */
#define FREE_PACKETS ((MAX_BUFFER_PACKETS - pkt_free + pkt_ready - 1) % MAX_BUFFER_PACKETS)
#define FREE_BUFFER ((PACKET_BUFFER_SIZE - pb_free + pb_ready - 1) % PACKET_BUFFER_SIZE)

/* called from syscall */
int josnic_allocate(int which)
{
	int result;
	
	if(which != -1)
	{
		if(which < 0)
			return -E_INVAL;
		if(josnic_devs <= which || !josnic_dev[which].valid)
			return -E_NO_DEV;
		if(josnic_dev[which].enabled)
			return -E_BUSY;
		
		result = josnic_dev[which].nic->open(josnic_dev[which].drv_which);
		if(!result)
			josnic_dev[which].enabled = 1;
		return result;
	}
	
	for(which = 0; which != josnic_devs; which++)
		if(josnic_dev[which].valid && !josnic_dev[which].enabled)
		{
			result = josnic_dev[which].nic->open(josnic_dev[which].drv_which);
			if(!result)
			{
				josnic_dev[which].enabled = 1;
				return which;
			}
		}
	
	return -E_NO_DEV;
}

int josnic_release(int which)
{
	int result;
	
	if(which < 0)
		return -E_INVAL;
	if(josnic_devs <= which || !josnic_dev[which].valid)
		return -E_NO_DEV;
	if(!josnic_dev[which].enabled)
		return -E_BUSY;
	
	result = josnic_dev[which].nic->close(josnic_dev[which].drv_which);
	if(!result)
		josnic_dev[which].enabled = 0;
	return result;
}

int josnic_get_address(int which, uint8_t * buffer)
{
	if(which < 0)
		return -E_INVAL;
	if(josnic_devs <= which || !josnic_dev[which].valid)
		return -E_NO_DEV;
	if(!josnic_dev[which].enabled)
		return -E_BUSY;
	
	return josnic_dev[which].nic->address(josnic_dev[which].drv_which, buffer);
}

int josnic_set_filter(int which, int flags)
{
	if(which < 0)
		return -E_INVAL;
	if(josnic_devs <= which || !josnic_dev[which].valid)
		return -E_NO_DEV;
	if(!josnic_dev[which].enabled)
		return -E_BUSY;
	
	return josnic_dev[which].nic->filter(josnic_dev[which].drv_which, flags);
}

int josnic_tx_reset(int which)
{
	if(which < 0)
		return -E_INVAL;
	if(josnic_devs <= which || !josnic_dev[which].valid)
		return -E_NO_DEV;
	if(!josnic_dev[which].enabled)
		return -E_BUSY;
	
	return josnic_dev[which].nic->reset(josnic_dev[which].drv_which);
}

int josnic_send_packet(int which, const void * data, int length)
{
	int i, result;
	uint32_t sum = 0;
	
	if(which < 0)
		return -E_INVAL;
	if(josnic_devs <= which || !josnic_dev[which].valid)
		return -E_NO_DEV;
	if(!josnic_dev[which].enabled)
		return -E_BUSY;
	
	/* pre-read the buffer to catch any user faults before sending the packet */
	for(i = 0; i < (length + 3) >> 2; i++)
		sum += ((uint32_t *) data)[i];
	
	result = josnic_dev[which].nic->transmit(josnic_dev[which].drv_which, data, length);
	if(!result)
		josnic_dev[which].trans_start = jiffies;
	return result;
}

int josnic_query(int which)
{
	if(which < 0)
		return -E_INVAL;
	if(josnic_devs <= which || !josnic_dev[which].valid)
		return -E_NO_DEV;
	if(!josnic_dev[which].enabled)
		return -E_BUSY;
	
	return READY_PACKETS;
}

int josnic_get_packet(int which, void * buffer, int length)
{
	if(which < 0)
		return -E_INVAL;
	if(josnic_devs <= which || !josnic_dev[which].valid)
		return -E_NO_DEV;
	if(!josnic_dev[which].enabled)
		return -E_BUSY;
	
	if(!READY_PACKETS)
		return -E_BUSY;
	
	if(josnic_pkt[pkt_ready].length < length)
		length = josnic_pkt[pkt_ready].length;
	
	/* Note that "buffer" may be a userspace buffer, in which case we might fault... */
	if(buffer)
		memcpy(buffer, &packet_buffer[josnic_pkt[pkt_ready].offset], length);
	
	length = (josnic_pkt[pkt_ready].length + 3) & ~0x3;
	
	/* free the buffer */
	//pb_ready = (pb_ready + length) % PACKET_BUFFER_SIZE;
	if((pb_ready += length) >= PACKET_BUFFER_SIZE) pb_ready = 0; /* the size fudging allows us to do this */
	length = josnic_pkt[pkt_ready].length;
	pkt_ready = (pkt_ready + 1) % MAX_BUFFER_PACKETS;
	
	return length;
}

/* called from drivers */
int josnic_register(const struct josnic * nic, int drv_which)
{
	if(josnic_devs == MAX_JOSNIC_DEVS)
		return -E_BUSY;
	
	josnic_dev[josnic_devs].nic = nic;
	josnic_dev[josnic_devs].valid = 1;
	josnic_dev[josnic_devs].enabled = 0;
	josnic_dev[josnic_devs].trans_start = jiffies;
	josnic_dev[josnic_devs].last_rx = jiffies;
	josnic_dev[josnic_devs].drv_which = drv_which;
	
	return josnic_devs++;
}

void * josnic_async_push_packet(int which, int length)
{
	int size = (length + 3) & ~0x3;
	
	if(which < 0)
		return NULL;
	if(josnic_devs <= which || !josnic_dev[which].valid)
		return NULL;
	if(!josnic_dev[which].enabled)
		return NULL;
	
	/* If this were a while loop, we could guarantee success below...
	 * but this way there is no chance of an infinite loop, and we
	 * won't drop fully received packets as often */
	if(!FREE_PACKETS || FREE_BUFFER < size)
	{
		printf("eth%d: Dropping packet from queue to make room for incoming packet\n", which);
		josnic_get_packet(which, NULL, 0);
	}
	if(FREE_PACKETS && FREE_BUFFER >= size)
	{
		int pn = pkt_free;
		pkt_free = (pkt_free + 1) % MAX_BUFFER_PACKETS;
		josnic_pkt[pn].offset = pb_free;
		//pb_free = (pb_free + size) % PACKET_BUFFER_SIZE;
		if((pb_free += size) >= PACKET_BUFFER_SIZE) pb_free = 0; /* the size fudging allows us to do this */
		josnic_pkt[pn].length = length;
		
		josnic_dev[which].last_rx = jiffies;
		return &packet_buffer[josnic_pkt[pn].offset];
	}
	
	printf("eth%d: Couldn't allocate a packet buffer of size %d\n", which, length);
	return NULL;
}

