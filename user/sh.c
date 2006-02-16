#include <inc/lib.h>

// TODO:
// - Opening a file over STDIN_FILENO to run commands from the file
//   means programs started by sh will have their STDIN_FILENO pointing
//   to the script file, too. This is ok for starting programs that do no
//   input, but this approach is wrong for apps that do use stdin.


#define BUFSIZ 1024		/* Find the buffer overrun bug! */
static int debug = 0;


// A place to stash a dup of stdin for cases when sh wants to make stdin
// a file to read from, but then wants spawned children to use sh's original
// stdin.
static int stdin_stash = 0;


// gettoken(s, 0) prepares gettoken for subsequent calls and returns 0.
// gettoken(0, token) parses a shell token from the previously set string,
// null-terminates that token, stores the token pointer in '*token',
// and returns a token ID (0, '<', '>', '|', '&', ';', '(', ')', or 'w').
// Subsequent calls to 'gettoken(0, token)' will return subsequent
// tokens from the string.
static int gettoken(char* s, char** token);


// Parse a shell command from string 's' and execute it.
// Do not return until the shell command is finished.
// runcmd() is called in a forked child,
// so it's OK to manipulate file descriptor state.
#define MAXARGS 16
static void
runcmd(char* s)
{
	char *argv[MAXARGS], *t, argv0buf[BUFSIZ];
	int argc, c, i, r, p[2], fd, pipe_child;
	bool start_in_bkgd;

	pipe_child = 0;
	gettoken(s, 0);
	// Find '&' using strchr(), rather than the below case statements,
	// because the below case statements would only allow the right most
	// command in a pipe's shell to detect the '&'. Detecting here
	// allows all commands' shells in a pipe sequence to detect this '&':
	start_in_bkgd = strchr(s, '&') ? 1 : 0;
	
again:
	argc = 0;
	while (1) {
		switch ((c = gettoken(0, &t))) {

		case 'w':	// Add an argument
			if (argc == MAXARGS) {
				kdprintf(STDERR_FILENO, "too many arguments\n");
				exit(0);
			}
			argv[argc++] = t;
			break;
			
		case '<':	// Input redirection
			// Grab the filename from the argument list
			if (gettoken(0, &t) != 'w') {
				kdprintf(STDERR_FILENO, "syntax error: < not followed by word\n");
				exit(0);
			}
			// Open 't' for reading as file descriptor 0
			// (which environments use as standard input).
			// We can't open a file onto a particular descriptor,
			// so open the file as 'fd',
			// then check whether 'fd' is 0.
			// If not, dup2 'fd' onto file descriptor 0,
			// then close the original 'fd'.
			
			fd = open(t, O_RDONLY);
			if(fd < 0)
			{
				kdprintf(STDERR_FILENO, "%s: %i\n", t, fd);
				exit(0);
			}
			if(fd)
			{
				r = dup2(fd, 0);
				if (r < 0)
				{
					kdprintf(STDERR_FILENO, "dup2: %i\n", r);
					exit(0);
				}
				r = close(fd);
				if (r < 0)
				{
					kdprintf(STDERR_FILENO, "close: %i\n", r);
					exit(0);
				}
				if (0 < stdin_stash)
				{
					close(stdin_stash);
					stdin_stash = 0;
				}
			}
				
			break;
			
		case '>':	// Output redirection
			// Grab the filename from the argument list
			if (gettoken(0, &t) != 'w') {
				kdprintf(STDERR_FILENO, "syntax error: > not followed by word\n");
				exit(0);
			}
			// Open 't' for writing as file descriptor 1
			// (which environments use as standard output).
			// We can't open a file onto a particular descriptor,
			// so open the file as 'fd',
			// then check whether 'fd' is 1.
			// If not, dup2 'fd' onto file descriptor 1,
			// then close the original 'fd'.
			
			fd = open(t, O_WRONLY | O_CREAT | O_TRUNC);
			if(fd < 0)
			{
				kdprintf(STDERR_FILENO, "%s: %i\n", t, fd);
				exit(0);
			}
			if(fd != 1)
			{
				dup2(fd, 1);
				close(fd);
			}
			
			break;
			
		case '|':	// Pipe
			// Set up pipe redirection.
			
			// Allocate a pipe by calling 'pipe(p)'.
			// Like the Unix version of pipe() (man 2 pipe),
			// this function allocates two file descriptors;
			// data written onto 'p[1]' can be read from 'p[0]'.
			// Then fork.
			// The child runs the right side of the pipe:
			//	Use dup2() to duplicate the read end of the pipe
			//	(p[0]) onto file descriptor 0 (standard input).
			//	Then close the pipe (both p[0] and p[1]).
			//	(The read end will still be open, as file
			//	descriptor 0.)
			//	Then 'goto again', to parse the rest of the
			//	command line as a new command.
			// The parent runs the left side of the pipe:
			//	Set 'pipe_child' to the child env ID.
			//	dup2() the write end of the pipe onto
			//	file descriptor 1 (standard output).
			//	Then close the pipe.
			//	Then 'goto runit', to execute this piece of
			//	the pipeline.

			r = pipe(p);
			if(r)
			{
				kdprintf(STDERR_FILENO, "pipe: %i\n", r);
				exit(0);
			}
			r = fork();
			if(r < 0)
			{
				kdprintf(STDERR_FILENO, "fork: %i\n", r);
				exit(0);
			}
			if(r)
			{
				/* parent */
				pipe_child = r;
				dup2(p[1], 1);
				close(p[0]);
				close(p[1]);
				goto runit;
			}
			/* child */
			dup2(p[0], 0);
			close(p[0]);
			close(p[1]);
			if(0 < stdin_stash)
			{
				close(stdin_stash);
				stdin_stash = 0;
			}
			goto again;
			
			break;

		case '&':		// Background start
			// Do nothing here, we detected '&' at the beginning of this fn
			break;

		case ';':
		case '(':
		case ')':
			printf("Unsupported shell token: %c\n", c);
			exit(0);
			break;

		case 0:		// String is complete
			// Run the current command!
			goto runit;
			
		default:
			printf("Unexpected shell token! (%c)\n", c);
			break;
			
		}
	}

runit:
	// Return immediately if command line was empty.
	if(argc == 0) {
		if (debug)
			printf("EMPTY COMMAND\n");
		return;
	}

	// Clean up command line.
	// Read all commands from the filesystem: add an initial '/' to
	// the command name (unless it starts with % for kernel binaries).
	// This essentially acts like 'PATH=/'.
	if (argv[0][0] != '/' && argv[0][0] != '%') {
		argv0buf[0] = '/';
		strcpy(argv0buf + 1, argv[0]);
		argv[0] = argv0buf;
	}
	argv[argc] = 0;
	
	// Print the command.
	if (debug) {
		printf("[%08x] SPAWN:", env->env_id);
		for (i = 0; argv[i]; i++)
			printf(" %s", argv[i]);
		printf("\n");
	}

	// Unstash sh's original stdin to pass on to child
	if (0 < stdin_stash)
	{
		r = dup2(stdin_stash, STDIN_FILENO);
		if (r < 0)
		{
			kdprintf(STDERR_FILENO, "dup2: %i\n", r);
			exit(0);
		}
		r = close(stdin_stash);
		if (r < 0)
		{
			kdprintf(STDERR_FILENO, "close: %i\n", r);
			exit(0);
		}
	}

	// Spawn the command!
	if ((r = spawn(argv[0], (const char **) argv)) < 0)
		kdprintf(STDOUT_FILENO, "spawn %s: %i\n", argv[0], r);

	// In the parent, close all file descriptors and wait for the
	// spawned command to exit.
	close_all();
	if (r >= 0) {
		if (debug)
			printf("[%08x] WAIT %s %08x\n", env->env_id, argv[0], r);
		if (!start_in_bkgd)
			wait(r);
		if (debug)
			printf("[%08x] wait finished\n", env->env_id);
	}

	// If we were the left-hand part of a pipe,
	// wait for the right-hand part to finish.
	if (pipe_child) {
		if (debug)
			printf("[%08x] WAIT pipe_child %08x\n", env->env_id, pipe_child);
		if (!start_in_bkgd)
			wait(pipe_child);
		if (debug)
			printf("[%08x] wait finished\n", env->env_id);
	}

	// Done!
	exit(0);
}


// Get the next token from string s.
// Set *p1 to the beginning of the token and *p2 just past the token.
// Returns
//	0 for end-of-string;
//	< for <;
//	> for >;
//	| for |;
//	w for a word.
//
// Eventually (once we parse the space where the \0 will go),
// words get nul-terminated.
#define WHITESPACE " \t\r\n"
#define SYMBOLS "<|>&;()"

static int
_gettoken(char* s, char** p1, char** p2)
{
	int t;

	if (s == 0) {
		if (debug > 1)
			printf("GETTOKEN NULL\n");
		return 0;
	}

	if (debug > 1)
		printf("GETTOKEN: %s\n", s);

	*p1 = 0;
	*p2 = 0;

	while (strchr(WHITESPACE, *s))
		*s++ = 0;
	if (*s == 0) {
		if (debug > 1)
			printf("EOL\n");
		return 0;
	}
	if (strchr(SYMBOLS, *s)) {
		t = *s;
		*p1 = s;
		*s++ = 0;
		*p2 = s;
		if (debug > 1)
			printf("TOK %c\n", t);
		return t;
	}
	*p1 = s;
	while (*s && !strchr(WHITESPACE SYMBOLS, *s))
		s++;
	*p2 = s;
	if (debug > 1) {
		t = **p2;
		**p2 = 0;
		printf("WORD: %s\n", *p1);
		**p2 = t;
	}
	return 'w';
}

static int
gettoken(char* s, char** p1)
{
	static int c, nc;
	static char* np1, *np2;

	if (s) {
		nc = _gettoken(s, &np1, &np2);
		return 0;
	}
	c = nc;
	*p1 = np1;
	nc = _gettoken(np2, &np1, &np2);
	return c;
}


static void
usage(void)
{
	printf("usage: sh [-dix] [command-file]\n");
	exit(0);
}

void
umain(int argc, char** argv)
{
	int r, interactive, echocmds;

	interactive = '?';
	echocmds = 0;
	ARGBEGIN{
	case 'd':
		debug++;
		break;
	case 'i':
		interactive = 1;
		break;
	case 'x':
		echocmds = 1;
		break;
	default:
		usage();
	}ARGEND

	if (argc > 1)
		usage();
	if (argc == 1) {
		// stash stdin to send to child
		stdin_stash = dup(STDIN_FILENO);
		if (stdin_stash < 0)
		{
			kdprintf(STDERR_FILENO, "dup2: %i\n", stdin_stash);
			exit(0);
		}

		r = close(STDIN_FILENO);
		if (r < 0)
		{
			kdprintf(STDERR_FILENO, "close: %i\n", r);
			exit(0);
		}
		if ((r = open(argv[0], O_RDONLY)) < 0)
		{
			kdprintf(STDERR_FILENO, "%s: %i", argv[0], r);
			exit(0);
		}
		assert(r==STDIN_FILENO);
	}
	if (interactive == '?')
		interactive = iscons(0);
	
	while (1) {
		char *buf;

		buf = readline(interactive ? "$ " : NULL);
		if (buf == NULL) {
			if (debug)
				printf("EXITING\n");
			if (interactive)
				printf("\n");
			exit(0);	// end of file
		}
		if (debug)
			printf("LINE: %s\n", buf);
		if (buf[0] == '#' || !buf[0])
			continue;
		if (echocmds)
			printf("# %s\n", buf);
		if (!strcmp(buf, "exit"))
			exit(0);
		if (debug)
			printf("BEFORE FORK\n");
		if ((r = fork()) < 0)
			panic("fork: %i", r);
		if (debug)
			printf("FORK: %d\n", r);
		if (r == 0) {
			runcmd(buf);
			exit(0);
		} else {
			if (!strchr(buf, '&'))
				wait(r);
		}
	}
}
