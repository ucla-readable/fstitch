#include <inc/lib.h>
#include <inc/mouse.h>

// commands we can send to the mouse 
#define CMD_RESET		0xFF
#define CMD_ENABLE		0xF4

// what the mouse says after it receives commands
#define ANS_ACK			0xFA
#define ANS_PASSED_SELF_TEST	0xAA
#define ANS_MOUSE_ID		0x00

// the nth bit of x
#define NTH_BIT(x, n) ((x & ((1 << (n + 1)) - 1)) >> n)

#define Y_OVERFLOW(x) NTH_BIT(x, 7)
#define X_OVERFLOW(x) NTH_BIT(x, 6)
#define Y_SIGN(x) NTH_BIT(x, 5)
#define X_SIGN(x) NTH_BIT(x, 4)
#define M_BUTTON(x) NTH_BIT(x, 2)
#define R_BUTTON(x) NTH_BIT(x, 1)
#define L_BUTTON(x) NTH_BIT(x, 0)

static int
mouse_reset(void)
{
	if (sys_mouse_ioctl(MOUSE_IOCTL_COMMAND, CMD_RESET, NULL) < 0)
		return -1;

	int state = 0;
	const uint8_t wanted[] = { ANS_ACK, ANS_PASSED_SELF_TEST, ANS_MOUSE_ID };
		// wanted[state] is what we are expecting
	while (1)
	{
		uint8_t b;
		if (sys_mouse_ioctl(MOUSE_IOCTL_READ, 1, &b) < 0)
			continue;
		if (b == wanted[state])
			state++;
		else if (b == wanted[0])
			state = 1;
		else
			state = 0;
		if (state == 3)
			break;
	}

	return 1;
}

static int
mouse_enable(void)
{
	uint8_t b;
	if (sys_mouse_ioctl(MOUSE_IOCTL_COMMAND, CMD_ENABLE, NULL) < 0)
		return -1;
	while (sys_mouse_ioctl(MOUSE_IOCTL_READ, 1, &b) < 0)
		;
	if (b != ANS_ACK)
		return -1;
	return 0;
}

static int16_t
mouse_displ(int overflow, int sign, uint8_t mantissa)
{
	uint16_t us = (sign ? 0xFF00 : 0x0000) | mantissa;
	int16_t r = *(int16_t *) &us;
	if (overflow)
	{
		if (sign)
			r -= 256;
		else
			r += 256;
	}
	return r;
}

static void
serve(int fd)
{
	uint8_t buf[3], pos = 0;
	while (1)
	{
		int n = sys_mouse_ioctl(MOUSE_IOCTL_READ, 3 - pos, buf + pos);
		if (n <= 0)
		{
			sys_yield();
			continue;
		}
		pos += n;
		if (pos == 3)
		{
			struct mouse_data data;
			data.dx = mouse_displ(X_OVERFLOW(buf[0]), X_SIGN(buf[0]), buf[1]);
			data.dy = mouse_displ(Y_OVERFLOW(buf[0]), Y_SIGN(buf[0]), buf[2]);
			data.left = L_BUTTON(buf[0]);
			data.middle = M_BUTTON(buf[0]);
			data.right = R_BUTTON(buf[0]);
			//printf("[dx=%d, dy=%d, buttons=%d%d%d]\n", data.dx, data.dy,
			//	data.left, data.middle, data.right);
			pos = 0;
			if (write(fd, &data, sizeof(data)) < 0)
				break;
		}
	}
}

void
umain(void)
{
	printf("Mouse Daemon ");

	if (sys_mouse_ioctl(MOUSE_IOCTL_DETECT, 0, NULL) < 0)
	{
		printf("failed: mouse not detected.\n");
		return;
	}
	else if (mouse_reset() < 0)
	{
		printf("failed: unable to reset the mouse.\n");
		return;
	}
	else
		printf("started.\n");

	if (fork() != 0)
		return;

	while (1)
	{
		envid_t client;
		int r = ipc_recv(0, &client, NULL, NULL, NULL, 0);
		if (r < 0)
			goto err_recv;

		r = mouse_enable();
		if (r < 0)
			goto err_enable;

		int fds[2];
		r = pipe(fds);
		if (r < 0)
			goto err_pipe;

		r = dup2env_send(fds[0], client);
		if (r < 0)
			goto err_send;


		close(fds[0]);
		serve(fds[1]);
		mouse_reset();
		continue;

		err_send:
			close(fds[0]);
			close(fds[1]);
		err_pipe:
			mouse_reset();
		err_enable:
			ipc_send(client, r, NULL, 0, NULL);
		err_recv:
			sys_yield();
	}
}

