#ifndef _JOSNIC_H_
#define _JOSNIC_H_ 1

#include <inc/types.h>

static uint16_t __inline__ htons(uint16_t data)
{
	return (data >> 8) | (data << 8);
}

static uint32_t __inline__ htonl(uint32_t data)
{
	return (data >> 24) | ((data >> 8) & 0xFF00) | ((data << 8) & 0xFF0000) | (data << 24);
}

#define ntohs htons
#define ntohl htonl

enum
{
	NET_IOCTL_ALLOCATE = 0,
	NET_IOCTL_RELEASE,
	NET_IOCTL_GETADDRESS,
	NET_IOCTL_SETFILTER,
	NET_IOCTL_RESET,
	NET_IOCTL_SEND,
	NET_IOCTL_QUERY,
	NET_IOCTL_RECEIVE
};

#endif
