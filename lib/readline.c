#include <inc/lib.h>
#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/error.h>

#define BUFLEN 1024
#define HIST_SIZE 10

static char hbuf[HIST_SIZE][BUFLEN] = {{0}};
static int hbuf_next = 0, htotal = 0;

static char buf[BUFLEN];

/* amazing how much simpler history is than command line editing */
static void add_history(char * line)
{
	if(*line)
	{
		memcpy(hbuf[hbuf_next++], line, BUFLEN);
		hbuf_next %= HIST_SIZE;
		if(htotal < HIST_SIZE)
			htotal++;
	}
}

static char * get_history(int index)
{
	if(htotal < index || index < 1 || HIST_SIZE < index)
		return NULL;
	return hbuf[(hbuf_next + HIST_SIZE - index) % HIST_SIZE];
}

static void repeat(int i, char c)
{
	for(; i; i--)
		putchar(c);
}

/* There are more efficient ways to handle command line editing, but this single
 * character at a time method has the nice side effect of automatically making
 * scrolling work properly. That is, entering or deleting text on one line of a
 * wrapped command causes the other lines to scroll properly, or the entire
 * screen to scroll if necessary. */
char * readline(const char * prompt)
{
	int i = 0, j = 0, c, echoing;
	int hindex = 0, hi = 0;
	char bbuf[BUFLEN];
	int overwrite = 0;
	
	if(prompt != NULL)
		printf("%s", prompt);
	
	echoing = iscons(0);
	for(;;)
	{
		c = getchar();
		switch(c)
		{
			case '\b':
			case 127:
				/* backspace */
				if(!j)
				{
					putchar('\a');
					break;
				}
				
				if(echoing)
				{
					int jj = j;
					while(jj < i)
						putchar(buf[jj++]);
					repeat(i - j + 1, '\b');
					for(jj = j; jj < i; jj++)
						putchar(buf[jj]);
					repeat(i - j, 127);
				}
				memmove(&buf[j - 1], &buf[j], i - j);
				i--;
				j--;
				break;
			case '\r':
				/* ignore \r because it is the devil's delimiter */
				break;
			case '\n':
			case KEYCODE_ENTER:
				if(echoing)
					putchar('\n');
				buf[i] = 0;
				add_history(buf);
				return buf;
			/* expanded keyboard driver allows special keys to be handled */
			/* ^P */
			case 'P' - '@':
			case KEYCODE_UP:
				if(hindex >= htotal)
				{
					putchar('\a');
					break;
				}
				
				if(!hindex)
				{
					memcpy(bbuf, buf, BUFLEN);
					hi = i;
				}
				
				if(echoing)
				{
					while(j < i)
						putchar(buf[j++]);
					repeat(i, '\b');
				}
				memcpy(buf, get_history(++hindex), BUFLEN);
				if(echoing)
					for(i = 0; buf[i]; i++)
						putchar(buf[i]);
				else
					i = strlen(buf);
				j = i;
				break;
			/* ^N */
			case 'N' - '@':
			case KEYCODE_DOWN:
				if(!hindex)
				{
					putchar('\a');
					break;
				}
				
				if(echoing)
				{
					while(j < i)
						putchar(buf[j++]);
					repeat(i, '\b');
				}
				
				if(--hindex)
				{
					memcpy(buf, get_history(hindex), BUFLEN);
					if(echoing)
						for(i = 0; buf[i]; i++)
							putchar(buf[i]);
					else
						i = strlen(buf);
				}
				else
				{
					memcpy(buf, bbuf, BUFLEN);
					if(echoing)
						for(i = 0; i != hi; i++)
							putchar(buf[i]);
					else
						i = hi;
				}
				j = i;
				break;
			/* ^B */
			case 'B' - '@':
			case KEYCODE_LEFT:
				if(j)
				{
					putchar(127);
					j--;
				}
				else
					putchar('\a');
				break;
			/* ^A */
			case 'A' - '@':
			case KEYCODE_HOME:
				repeat(j, 127);
				j = 0;
				break;
			/* ^F */
			case 'F' - '@':
			case KEYCODE_RIGHT:
				if(j < i)
					putchar(buf[j++]);
				else
					putchar('\a');
				break;
			/* ^E */
			case 'E' - '@':
			case KEYCODE_END:
				while(j < i)
					putchar(buf[j++]);
				break;
			/* ^K */
			case 'K' - '@':
				/* erase to end of line */
				if(echoing)
				{
					int jj = j;
					while(jj < i)
						putchar(buf[jj++]);
					repeat(i - j, '\b');
				}
				i = j;
				break;
			case 'U' - '@':
				/* erase to beginning of line */
				if(echoing)
				{
					int jj = j;
					while(jj < i)
						putchar(buf[jj++]);
					repeat(i, '\b');
					for(jj = j; jj < i; jj++)
						putchar(buf[jj]);
					repeat(i - j, 127);
				}
				memmove(buf, &buf[j], i - j);
				i -= j;
				j = 0;
				break;
			case KEYCODE_INSERT:
				overwrite = !overwrite;
				break;
			default:
				if(c < 0)
				{
					if(c != -E_EOF)
#ifdef KUDOS_KERNEL
						printf("read error: %e\n", c);
#else
						kdprintf(STDERR_FILENO, "read error: %e\n", c);
#endif
					return NULL;
				}
				else if(c >= ' ' && c <= '~' && overwrite && j < BUFLEN - 1)
				{
					if(echoing)
						putchar(c);
					buf[j++] = c;
					if(j > i)
						i = j;
				}
				else if(c >= ' ' && c <= '~' && i < BUFLEN - 1)
				{
					if(echoing)
					{
						int jj = j;
						putchar(c);
						while(jj < i)
							putchar(buf[jj++]);
						repeat(i - j, 127);
					}
					memmove(&buf[j + 1], &buf[j], i - j);
					buf[j++] = c;
					i++;
				}
				else
					putchar('\a');
				break;
		}
	}
}
