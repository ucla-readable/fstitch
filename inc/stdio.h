#ifndef KUDOS_INC_STDIO_H
#define KUDOS_INC_STDIO_H

#include <inc/stdarg.h>

#ifndef NULL
#define NULL ((void*)0)
#endif /* !NULL */

#define KEYCODE_UP 184
#define KEYCODE_DOWN 178
#define KEYCODE_LEFT 180
#define KEYCODE_RIGHT 182
#define KEYCODE_HOME 183
#define KEYCODE_END 177
#define KEYCODE_INSERT 176
#define KEYCODE_DELETE 174
#define KEYCODE_PGUP 185
#define KEYCODE_PGDOWN 179
#define KEYCODE_ENTER 138

// lib/stdio.c
void	putchar(int c);
int	getchar(void);
int	iscons(int fd);

// lib/printfmt.c
void	printfmt(void (*putch)(int, void*), void* putdat, const char* fmt, ...);
void	vprintfmt(void (*putch)(int, void*), void* putdat, const char* fmt, va_list ap);

// lib/printf.c
#ifndef KUDOS_KERNEL
int	printf_c(const char*, ...);
#else
int	printf(const char*, ...);
#endif
int	vprintf(const char*, va_list);

// lib/sprintf.c
int	snprintf(char*, int, const char*, ...);
int	vsnprintf(char*, int, const char*, va_list);

// Standard file descriptors.
#define  STDIN_FILENO   0  // Standard input.
#define  STDOUT_FILENO  1  // Standard output.
#define  STDERR_FILENO  2  // Standard error output.

// lib/fprintf.c
#ifndef KUDOS_KERNEL
int   printf(const char *fmt, ...);
#endif
int	fprintf(int fd, const char*, ...);
int	vfprintf(int fd, const char*, va_list);

// lib/readline.c
char*	readline(const char* prompt);

#endif /* !KUDOS_INC_STDIO_H */
