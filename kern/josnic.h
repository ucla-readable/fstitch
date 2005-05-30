#ifndef KUDOS_KERN_JOSNIC_H
#define KUDOS_KERN_JOSNIC_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

struct josnic {
	int (*open)(int drv_which);
	int (*close)(int drv_which);
	int (*address)(int drv_which, uint8_t * buffer);
	int (*transmit)(int drv_which, const void * data, int length);
	int (*filter)(int drv_which, int flags);
	int (*reset)(int drv_which);
};

/* called from syscall */
int josnic_allocate(int which);
int josnic_release(int which);
int josnic_get_address(int which, uint8_t * buffer);
int josnic_set_filter(int which, int flags);
int josnic_tx_reset(int which);
int josnic_send_packet(int which, const void * data, int length);
int josnic_query(int which);
int josnic_get_packet(int which, void * data, int length);

/* called from drivers */
int josnic_register(const struct josnic * nic, int drv_which);
void * josnic_async_push_packet(int which, int length);

#endif /* !KUDOS_KERN_JOSNIC_H */
