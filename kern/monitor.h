#ifndef KUDOS_KERN_MONITOR_H
#define KUDOS_KERN_MONITOR_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

struct Trapframe;

// Activate the kernel monitor, optionally providing a trap
// frame indicating the current state (NULL if none).
void monitor(struct Trapframe *tf);

// Functions implementing monitor commands.
int mon_help(int argc, char **argv, struct Trapframe * tf);
int mon_kerninfo(int argc, char **argv, struct Trapframe * tf);

int mon_breakpoint(int argc, char **argv, struct Trapframe *tf);

int mon_backtrace(int argc, char **argv, struct Trapframe * tf);
int mon_symbols(int argc, char **argv, struct Trapframe *tf);

int mon_page_alloc(int argc, char **argv, struct Trapframe * tf);
int mon_page_free(int argc, char **argv, struct Trapframe * tf);
int mon_page_status(int argc, char **argv, struct Trapframe * tf);
int mon_page_map(int argc, char ** argv, struct Trapframe * tf);
int mon_page_unmap(int argc, char ** argv, struct Trapframe * tf);

int mon_show_page_maps(int argc, char ** argv, struct Trapframe * tf);
int mon_set_dir_perm(int argc, char ** argv, struct Trapframe * tf);
int mon_set_page_perm(int argc, char ** argv, struct Trapframe * tf);
int mon_dump_mem(int argc, char ** argv, struct Trapframe * tf);

int mon_env_list(int argc, char ** argv, struct Trapframe * tf);
int mon_env_current(int argc, char ** argv, struct Trapframe * tf);
int mon_env_priority(int argc, char ** argv, struct Trapframe * tf);
int mon_env_run(int argc, char ** argv, struct Trapframe * tf);
int mon_env_kill(int argc, char ** argv, struct Trapframe * tf);

int mon_env_debug(int argc, char ** argv, struct Trapframe * tf);

int mon_init(int argc, char ** argv, struct Trapframe * tf);

int mon_exit(int argc, char ** argv, struct Trapframe * tf);

#endif	// !KUDOS_KERN_MONITOR_H
