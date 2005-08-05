#ifndef KUDOS_INC_CONFIG_H
#define KUDOS_INC_CONFIG_H

#define RELEASE_NAME "cornsyrup"

//
// Formatting options

#define CLASS_FORMAT 0

#define CLASS_TF_FORMAT      CLASS_FORMAT
#define CLASS_WELCOME_FORMAT CLASS_FORMAT


//
// Functional options

// Set to use env symbol tables.
#define ENABLE_ENV_SYMS 1

// Set to enable interrupts while within the kernel.
#define ENABLE_INKERNEL_INTS 1

// Set to enable env fpu support.
#define ENABLE_ENV_FP 1

// Use the first serial port (if any are found) for console usage.
#define ENABLE_SERIAL_CONSOLE 1

// Send console output to the parallel port
#define ENABLE_PARALLEL_CONSOLE_OUTPUT 1


//
// Defaults

// Set to enable dhcp on josnic interfaces.
#define ENABLE_JOSNIC_DHCP 1

// DEFAULT_IP_JOSNIC used when:
// - ENABLE_JOSNIC_DHCP==0
// - Some of addr, netmask, gw, dns are passed via the command line (dhcp then not used)
#define DEFAULT_IP_JOSNIC_ADDR    "192.168.3.2"
#define DEFAULT_IP_JOSNIC_NETMASK "255.255.255.0"
#define DEFAULT_IP_JOSNIC_GW      "192.168.3.1"
#define DEFAULT_IP_JOSNIC_DNS     "128.143.2.7"

#define DEFAULT_IP_SLIP_ADDR      "192.168.2.2"
#define DEFAULT_IP_SLIP_NETMASK   "255.255.255.255"
#define DEFAULT_IP_SLIP_GW        "192.168.2.1"
#define DEFAULT_IP_SLIP_DNS       "128.143.2.7"

#endif // !KUDOS_INC_CONFIG_H
