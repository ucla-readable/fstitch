#ifndef _MOUSE_H_
#define _MOUSE_H_ 1

enum
{
	MOUSE_IOCTL_DETECT = 0,
	MOUSE_IOCTL_READ,
	MOUSE_IOCTL_COMMAND
};

struct mouse_data
{
	int16_t dx;
	int16_t dy;
	uint8_t left:1, middle:1, right:1;
};

#endif
