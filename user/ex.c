// ex manuals you that be useful references:
// http://www.comp.lancs.ac.uk/computing/users/eiamjw/unix/chap9.html
// TODO: http://www.ungerhu.com/jxh/vi.html
// TODO: command addresses: http://www.scit.wlv.ac.uk/cgi-bin/mansec?1+ex

#include <types.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <mmu.h>
#include <error.h>
#include <vector.h>
#include <malloc.h>
#include <lib.h>

static const char prompt[] = ":";

typedef struct line {
	struct line * prev, * next;
	char * text;
} line_t;

typedef void (*cmdfn)(size_t begin_lineno, size_t end_lineno, const char * cmd);

typedef struct {
	cmdfn f;
	char * description;
} cmd_entry_t;

#define CMD_BEGIN '#'
#define CMD_END   'z'
#define CMDS_SIZE ((int)(CMD_END - CMD_BEGIN) + 1)
static cmd_entry_t cmds[CMDS_SIZE];

#define CMDX(c) ((int)(c - CMD_BEGIN))

typedef struct {
	int fdnum;
	char * filename;
	bool modified;

	size_t   cur_lineno;
	line_t * cur_line;
	line_t * first_line;
	size_t   numlines; // number of lines in this file
} ex_file_t;

static ex_file_t ex_file;

static void ex_file_init(ex_file_t * f)
{
	f->fdnum      = -1;
	f->filename   = NULL;
	f->modified   = 0;
	f->cur_lineno = 1;
	f->cur_line   = NULL;
	f->first_line = NULL;
	f->numlines   = 0;
}


//
// Line use

static line_t * line_create(char * text, line_t * prev)
{
	line_t * l = malloc(sizeof(*l));
	if (!l)
		return NULL;

	l->text = text;
	l->prev = prev;
	if (prev)
		l->next = prev->next;
	else
		l->next = NULL;

	if (l->prev)
		l->prev->next = l;
	if (l->next)
		l->next->prev = l;

	return l;
}

static size_t mmod(int x, size_t y)
{
	int z = x % (int)y;
	while (z < 0)
		z += y;
	return z;
}

static line_t * line_get(size_t lineno)
{
	if (!ex_file.numlines)
		return NULL;

	if (!lineno)
		lineno = 1;

	// Shift down one for use in mod
	const int ln = lineno - 1;
	const int cln = ex_file.cur_lineno - 1;

	const size_t cur_fwd_dist   = mmod(ln - cln, ex_file.numlines);
	const size_t cur_bwd_dist   = mmod(cln - ln, ex_file.numlines);
	const size_t first_fwd_dist = mmod(ln - 0,   ex_file.numlines);
	const size_t first_bwd_dist = mmod(0 - ln,   ex_file.numlines);
	size_t fwd_dist, bwd_dist;
	line_t * l;

	if (lineno > ex_file.numlines)
		return NULL;
	if (MIN(cur_fwd_dist, cur_bwd_dist) <= MIN(first_fwd_dist, first_bwd_dist))
	{
		fwd_dist = cur_fwd_dist;
		bwd_dist = cur_bwd_dist;
		l = ex_file.cur_line;
	}
	else
	{
		fwd_dist = first_fwd_dist;
		bwd_dist = first_bwd_dist;
		l = ex_file.first_line;
	}

	if (fwd_dist <= bwd_dist)
	{
		size_t i = fwd_dist;
		while (i-- > 0)
			l = l->next;
	}
	else
	{
		size_t i = bwd_dist;
		while (i-- > 0)
			l = l->prev;
	}

	return l;
}

static int line_goto(size_t lineno)
{
	line_t * l;

	if (!lineno)
		lineno = 1;

	if (!ex_file.numlines)
	{
		if (lineno == 1)
			return 0;
		else
			return -E_INVAL;
	}

	l = line_get(lineno);
	if (!l)
		return -E_INVAL;

	ex_file.cur_line = l;
	ex_file.cur_lineno = lineno;
	return 0;
}

static int line_go(int offset)
{
	return line_goto(ex_file.cur_lineno + offset);
}

/* Not in use
static void line_swap(line_t * l1, line_t * l2)
{
	line_t * tmp;

	tmp = l1->next;
	l1->next = l2->next;
	l2->next = tmp;

	tmp = l1->prev;
	l1->prev = l2->prev;
	l2->prev = tmp;

	l1->prev->next = l1;
	l1->next->prev = l1;
	l2->prev->next = l2;
	l2->next->prev = l2;
}
*/

static int line_insert(char * text)
{
	line_t * l = line_create(text, NULL);
	if (!l)
		return -E_NO_MEM;

	if (ex_file.numlines)
	{
		l->next = ex_file.cur_line;
		l->prev = ex_file.cur_line->prev;

		l->next->prev = l;
		l->prev->next = l;
	}
	else
	{
		l->next = l;
		l->prev = l;
		ex_file.cur_lineno = 1;
	}

	ex_file.numlines++;
	if (ex_file.cur_line == ex_file.first_line)
		ex_file.first_line = l;
	ex_file.cur_line = l;
	ex_file.modified = 1;
	return 0;
}

static int line_append(char * text)
{
	line_t * l = line_create(text, ex_file.cur_line);
	if (!l)
		return -E_NO_MEM;

	if (!ex_file.cur_line)
	{
		l->next = l;
		l->prev = l;
		ex_file.first_line = l;
		ex_file.cur_lineno = 0; // so that ++ will make it 1
	}

	ex_file.numlines++;
	ex_file.cur_lineno++;
	ex_file.cur_line = l;
	ex_file.modified = 1;
	return 0;
}

static void line_delete(void)
{
	line_t * l = ex_file.cur_line;
	if (!l)
		return;

	if (ex_file.first_line == ex_file.cur_line)
		ex_file.first_line = l->next;
	ex_file.cur_line = l->next;

	free(l->text);
	if (l->prev)
		l->prev->next = l->next;
	if (l->next)
		l->next->prev = l->prev;
	free(l);

	ex_file.numlines--;
	if (!ex_file.numlines)
	{
		ex_file.first_line = NULL;
		ex_file.cur_line = NULL;
	}
	ex_file.modified = 1;
}

static int line_move(size_t target_lineno)
{
	line_t * orig_line;
	size_t orig_lineno;
	char * orig_text;
	int r;

	if (ex_file.cur_lineno == target_lineno)
		return 0;

	orig_lineno = ex_file.cur_lineno;
	orig_line = ex_file.cur_line;
	orig_text = strdup(ex_file.cur_line->text);
	if (!orig_text)
		return -E_NO_MEM;

	r = line_goto(target_lineno);
	if (r < 0)
		return r;
	r = line_append(orig_text);
	if (r < 0)
		return r;

	if (target_lineno < orig_lineno)
	{
		orig_lineno++;
		target_lineno++;
	}
	r = line_goto(orig_lineno);
	assert(r >= 0);
	line_delete();
	r = line_goto(target_lineno);
	assert(r >= 0);

	ex_file.modified = 1;
	return 0;
}

//
// Parsing

// Error: -1 lineno before beginning of file, -2 after end of file
static int parse_lineno(const char ** x, size_t *line)
{
	const char * c = *x;

	if ('.' == *c)
	{
		*line = ex_file.cur_lineno;
		c++;
	}
	else if ('$' == *c)
	{
		if (ex_file.numlines)
			*line = ex_file.numlines;
		else
			*line = 1;
		c++;
	}
	else if (isnum(*c) || '+' == *c || '-' == *c)
	{
		int offset = 0;

		if ('+' == *c)
		{
			offset = 1;
			c++;
		}
		else if ('-' == *c)
		{
			offset = -1;
			c++;
		}

		const size_t l = strtol(c, (char**) &c, 10);
		if (!offset)
		{
			if (l)
				*line = l;
			else
				*line = 1;
		}
		else
		{
			if (-1 == offset && l > ex_file.cur_lineno)
				return -1;
			if (1 == offset && l+ex_file.cur_lineno > ex_file.numlines)
				return -2;

			*line = offset*l + ex_file.cur_lineno;
		}
	}
	else
	{
		// Set begin and end to cur_lineno for cmds without explicit lineno
		*line = ex_file.cur_lineno;
	}

	*x = c;
	return 0;
}

// Same errors as parse_lineno
static int parse_linenos(const char ** x, size_t * begin, size_t * end)
{
	const char * c = *x;
	size_t b, e;
	int r;

	r = parse_lineno(&c, &b);
	if (r < 0)
		return r;
	
	if (',' == *c)
	{
		c++;
		if (('0' <= *c && *c <= '9') || *c == '$' || *c == '.' || *c == '+' || *c == '-')
		{
			r = parse_lineno(&c, &e);
			if (r < 0)
				return r;
		}
		else
			e = ex_file.numlines;
	}
	else
		e = b;

	if (b > e)
	{
		fprintf(STDERR_FILENO, "Invalid lineno range %u-%u\n", b, e);
		return -E_INVAL;
	}

	*x = c;
	*begin = b;
	*end = e;
	return 0;
}

static const char * parse_filename(const char * cmd)
{
	if (!cmd[1])
		return NULL;

	while (*cmd == ' ')
		cmd++;
	if (!*cmd)
		return NULL;
	return cmd;
}


//
// File operations

static char file_read_buf[PGSIZE];
static int file_insert(const char * file, int mode)
{
	int n, r;
	// for multi-buf lines:
	vector_t * line; // vector_t of char*, each entry is a part of the cur line
	size_t line_len = 0; // total length of partial lines
	int fd;

	fd = open(file, mode);
	if (fd < 0)
		return fd;

	line = vector_create();
	if (!line)
	{
		(void) close(fd); // ignore possible error
		return -E_NO_MEM;
	}

	while ((n = read(fd, file_read_buf, sizeof(file_read_buf))))
	{
		int line_begin, i;
		for (i=0, line_begin=0; i < n; i++)
		{
			if (file_read_buf[i] == '\n')
			{
				char * l = malloc(i - line_begin + 1);
				assert(l);
				memcpy(l, &file_read_buf[line_begin], i-line_begin);
				l[i - line_begin] = 0;
				
				if (vector_size(line))
				{
					int j;
					char * l_cur;

					// add l to line
					r = vector_push_back(line, l);
					assert(r >= 0);
					line_len += i - line_begin;

					// build complete line from the partial lines
					l = malloc(line_len);
					assert(l);
					l_cur = l;
					for (j=0; j < vector_size(line); j++)
					{
						strcpy(l_cur, vector_elt(line, j));
						l_cur += strlen(vector_elt(line, j));
						free(vector_elt(line, j));
					}
					vector_clear(line);
					line_len = 0;
				}

				r = line_append(l);
				assert(r >= 0);

				line_begin = i+1;
			}
			else if (i+1 == n)
			{
				// store partial line
				char * l = malloc(i - line_begin + 1);
				assert(l);
				memcpy(l, &file_read_buf[line_begin], i-line_begin);
				l[i - line_begin] = 0;

				r = vector_push_back(line, l);
				assert(r >= 0);

				line_len += i - line_begin;
			}
		}
	}

	vector_destroy(line);

	return fd;
}

static int file_save(void)
{
	line_t * l = ex_file.first_line;
	bool first = 1;
	int r;

	if (ex_file.fdnum == -1)
		return -E_NOT_FOUND;

	// replace the existing file (slow but easy)

	r = ftruncate(ex_file.fdnum, 0);
	if (r < 0)
	{
		fprintf(STDERR_FILENO, "%s(): %s: %e\n", __FUNCTION__, "ftrucate", r);
		return r;
	}

	r = seek(ex_file.fdnum, 0);
	if (r < 0)
	{
		fprintf(STDERR_FILENO, "%s(): %s: %e\n", __FUNCTION__, "seek", r);
		return r;
	}

	if (!l)
		return 0;

	while (first || l != ex_file.first_line)
	{
		first = 0;

		r = write(ex_file.fdnum, l->text, strlen(l->text));
		if (r < 0)
		{
			fprintf(STDERR_FILENO, "%s(): %s: %e\n", __FUNCTION__, "write", r);
			return r;
		}
		fprintf(ex_file.fdnum, "\n");
		l = l->next;
	}

	return 0;
}

static int file_close(void)
{
	int r;

	if (ex_file.fdnum == -1 || !ex_file.numlines)
		return 0;

	r = close(ex_file.fdnum);
	if (r < 0)
		return r;

	(void) line_goto(1);
	while (ex_file.numlines)
		line_delete();

	free(ex_file.filename);
	ex_file_init(&ex_file);
	return 0;
}

static int file_open(const char * file, int mode)
{
	int fd, r;

	assert(ex_file.fdnum == -1); // ex_file must be closed
	fd = file_insert(file, mode);
	if (fd < 0)
		return fd;
	ex_file.fdnum = fd;

	ex_file.modified = 0;
	ex_file.filename = strdup(file);
	assert(ex_file.filename);

	struct Stat s;
	r = fstat(ex_file.fdnum, &s);
	assert(r >= 0);
	printf("\"%s\" %uL, %uC\n", file, ex_file.numlines, s.st_size);

	return 0;
}


//
// Commands

static void cmd_shell(size_t begin, size_t end, const char * cmd)
{
	int r;

	if (begin != ex_file.cur_lineno || end != ex_file.cur_lineno)
	{
		fprintf(STDERR_FILENO, "No range allowed\n");
		return;
	}

	r = spawnl("/sh", "/sh", (const char **) 0);
	if (r < 0)
		fprintf(STDERR_FILENO, "spawn /sh: %e\n", r);
	wait(r);
}

static void cmd_quit(size_t begin, size_t end, const char * cmd)
{
	int r;

	if (begin != ex_file.cur_lineno || end != ex_file.cur_lineno)
	{
		fprintf(STDERR_FILENO, "No range allowed\n");
		return;
	}

	if (ex_file.modified && cmd[1] != '!')
	{
		fprintf(STDERR_FILENO, "No write since last change (use ! to override)\n");
		return;
	}

	r = file_close();
	if (r < 0)
		fprintf(STDERR_FILENO, "%s(): %s: %e\n", __FUNCTION__, "file_close", r);
	exit();
}

static void cmd_insert_file(size_t begin, size_t end, const char * cmd)
{
	const char * file = NULL;
	int r;

	if (begin != ex_file.cur_lineno || end != ex_file.cur_lineno)
	{
		fprintf(STDERR_FILENO, "Range write not implemented\n");
		return;
	}

	file = parse_filename(cmd+1);
	if (!file)
	{
		fprintf(STDERR_FILENO, "No filename given\n");
		return;
	}

	r = file_insert(file, O_RDWR);
	if (r < 0)
	{
		fprintf(STDERR_FILENO, "Unable to insert file \"%s\": %e\n", file, r);
		return;
	}
	r = close(r);
	if (r < 0)
		panic("%s(): close: %e\n", __FUNCTION__, r);
}

static int write_file(const char * file)
{
	int r;

	if (file)
	{
		bool file_was_open = ex_file.fdnum != -1;
		if (file_was_open)
		{
			r = close(ex_file.fdnum);
			assert(r >= 0);
		}
		ex_file.fdnum = open(file, O_RDWR|O_CREAT);
		if (ex_file.fdnum < 0)
		{
			fprintf(STDERR_FILENO, "Unable to write to \"%s\"\n", file);
			if (file_was_open)
			{
				ex_file.fdnum = open(ex_file.filename, O_RDWR);
				if (ex_file.fdnum < 0)
				{
					ex_file.fdnum = -1;
					fprintf(STDERR_FILENO, "Unable to reopen original file\n");
				}
			}
			return -E_UNSPECIFIED;
		}
		if (file_was_open)
			free(ex_file.filename);
		ex_file.filename = strdup(file);
		assert(ex_file.filename);
	}

	r = file_save();
	if (r < 0)
	{
		fprintf(STDERR_FILENO, "%s(): %s: %e\n", __FUNCTION__, "file_save", r);
		return -E_UNSPECIFIED;
	}

	ex_file.modified = 0;
	return 0;
}

static void cmd_write(size_t begin, size_t end, const char * cmd)
{
	const char * file = NULL;
	int r;

	if (begin != ex_file.cur_lineno || end != ex_file.cur_lineno)
	{
		fprintf(STDERR_FILENO, "Range write not implemented\n");
		return;
	}

	file = parse_filename(cmd+1);
	if (!file)
	{
		if (cmd[1])
		{
			fprintf(STDERR_FILENO, "No filename given\n");
			return;
		}
		file = NULL;
	}

	r = write_file(file);
	if (r < 0)
		return;

	if (cmd[1] == 'q')
	{
		r = file_close();
		if (r < 0)
			fprintf(STDERR_FILENO, "%s(): %s: %e\n", __FUNCTION__, "file_close", r);
		exit();
	}

	struct Stat s;
	r = fstat(ex_file.fdnum, &s);
	printf("\"%s\" %uL, %uC written\n", ex_file.filename, ex_file.numlines, s.st_size);
}

static void cmd_writequit(size_t begin, size_t end, const char * cmd)
{
	const char * file = NULL;
	int r;

	if (begin != ex_file.cur_lineno || end != ex_file.cur_lineno)
	{
		fprintf(STDERR_FILENO, "Range write not implemented\n");
		return;
	}

	file = parse_filename(cmd+1);
	if (!file)
	{
		if (cmd[1])
		{
			fprintf(STDERR_FILENO, "No filename given\n");
			return;
		}
		file = ex_file.filename;
	}

	r = write_file(file);
	if (r < 0)
		return;

	cmd_quit(begin, end, cmd);
}

static void cmd_display_file(size_t begin, size_t end, const char * cmd)
{
	if (ex_file.filename)
		printf("\"%s\" ", ex_file.filename);
	else
		printf("\"[No File]\" ");

	if (ex_file.numlines)
		printf("%u lines --%d%%--\n", ex_file.numlines, (100*ex_file.cur_lineno) / ex_file.numlines);
	else
		printf("--No lines in buffer--\n");
}

static void cmd_set_lineno(size_t begin, size_t end, const char * cmd)
{
	size_t prev_lineno = ex_file.cur_lineno;
	int r;

	if (begin != end)
	{
		printf("Current lineno can not be a range\n");
		return;
	}

	r = line_goto(begin);
	if (r < 0)
	{
		printf("Lineno %u out of range (file %u lines)\n", begin, ex_file.numlines);
		return;
	}

	if (prev_lineno != ex_file.cur_lineno)
		printf("%s\n", ex_file.cur_line->text);
}

static void cmd_get_lineno(size_t begin, size_t end, const char * cmd)
{
	int r;

	if (begin != end)
	{
		printf("Current lineno can not be a range\n");
		return;
	}

	r = line_goto(begin);
	if (r < 0)
	{
		printf("Lineno %u out of range (file %u lines)\n", begin, ex_file.numlines);
		return;
	}

	printf("line %u\n", ex_file.cur_lineno);
}

static void cmd_transfer(size_t begin, size_t end, const char * cmd)
{
	size_t target_begin, target_end;
	const char * target_string = cmd + 1;
	size_t i, j;
	int r;

	r = parse_linenos(&target_string, &target_begin, &target_end);
	if (r < 0)
	{
		printf("Illegal target linenos: %e\n", r);
		return;
	}
	if (target_begin != target_end)
	{
		printf("Transfer does not support transferring to ranges of lines\n");
		return;
	}

	if (begin > ex_file.numlines)
	{
		printf("Lineno %u out of range (file %u lines)\n", begin, ex_file.numlines);
		return;
	}
	if (end > ex_file.numlines)
	{
		printf("Lineno %u out of range (file %u lines)\n", end, ex_file.numlines);
		return;
	}
	if (target_begin > ex_file.numlines)
	{
		printf("Lineno %u out of range (file %u lines)\n", target_begin, ex_file.numlines);
		return;
	}
	if (target_end > ex_file.numlines)
	{
		printf("Lineno %u out of range (file %u lines)\n", target_end, ex_file.numlines);
		return;
	}

	for (i=0,j=0; begin+i <= end; i++, j++)
	{
		char * text;

		if (i>0 && target_begin < begin)
			begin++;

		r = line_goto(begin+j);
		assert(r >= 0);
		text = strdup(ex_file.cur_line->text);
		assert(text);

		r = line_goto(target_begin+j);
		assert(r >= 0);
		r = line_append(text);
		if (r < 0)
		{
			printf("%s(): %s: %e\n", __FUNCTION__, "line_move", r);
			return;
		}
	}

	printf("%s\n", ex_file.cur_line->text);
}

static void cmd_move(size_t begin, size_t end, const char * cmd)
{
	size_t target_begin, target_end;
	const char * target_string = cmd + 1;
	size_t i, j;
	int r;

	r = parse_linenos(&target_string, &target_begin, &target_end);
	if (r < 0)
	{
		printf("Illegal target linenos: %e\n", r);
		return;
	}
	if (target_begin != target_end)
	{
		printf("Move does not support moving to ranges of lines\n");
		return;
	}

	if (begin > ex_file.numlines)
	{
		printf("Lineno %u out of range (file %u lines)\n", begin, ex_file.numlines);
		return;
	}
	if (end > ex_file.numlines)
	{
		printf("Lineno %u out of range (file %u lines)\n", end, ex_file.numlines);
		return;
	}
	if (target_begin > ex_file.numlines)
	{
		printf("Lineno %u out of range (file %u lines)\n", target_begin, ex_file.numlines);
		return;
	}
	if (target_end > ex_file.numlines)
	{
		printf("Lineno %u out of range (file %u lines)\n", target_end, ex_file.numlines);
		return;
	}

	for (i=0,j=0; begin+i <= end; i++, j++)
	{
		if (i>0 && begin < target_begin)
			j--;

		r = line_goto(begin+j);
		assert(r >= 0);
		r = line_move(target_begin+j);
		if (r < 0)
		{
			printf("%s(): %s: %e\n", __FUNCTION__, "line_move", r);
			return;
		}
	}

	printf("%s\n", ex_file.cur_line->text);
}

static void display_lines(size_t begin, size_t end, bool linenos)
{
	line_t * l;
	size_t i = begin;
	int r;

	r = line_goto(begin);
	assert(r >= 0);

	l = ex_file.cur_line;

	for (i=begin; i <= end; i++)
	{
		const size_t len = strlen(ex_file.cur_line->text);
		if (linenos)
			printf("\t%u ", i); // TODO: left align number
		r = write(STDOUT_FILENO, ex_file.cur_line->text, len);
		if (r < len)
		{
			if (r < 0)
				fprintf(STDOUT_FILENO, "%s(): %s: %e\n", __FUNCTION__, "write", r);
			else
				fprintf(STDOUT_FILENO, "%s(): Only able to display %u of %u chars on line\n", __FUNCTION__, r, len);
			return;
		}
		printf("\n");

		if (i < end)
		{
			r = line_go(1);
			assert(r >= 0);
		}
	}
}

static void cmd_display_lines(size_t begin, size_t end, const char * cmd)
{
	display_lines(begin, end, 0);
}

static void cmd_display_lines_linenos(size_t begin, size_t end, const char * cmd)
{
	display_lines(begin, end, 1);
}

static void cmd_insert(size_t begin, size_t end, const char * cmd)
{
	char * text;
	int r;
	bool first = 1;

	r = line_goto(begin);
	if (r < 0)
	{
		fprintf(STDOUT_FILENO, "Illegal lineno\n");
		return;
	}

	while (*(text = readline("")) != '.')
	{
		text = strdup(text);
		assert(text);
		if (first)
		{
			r = line_insert(text);
			first = 0;
		}
		else
			r = line_append(text);
		assert(r >= 0);
	}
}

static void cmd_append(size_t begin, size_t end, const char * cmd)
{
	char * text;
	int r;

	// FIXME: "0a" should be equiv to "1i"

	r = line_goto(begin);
	if (r < 0)
	{
		fprintf(STDOUT_FILENO, "Illegal lineno\n");
		return;
	}

	while (*(text = readline("")) != '.')
	{
		text = strdup(text);
		assert(text);
		r = line_append(text);
		assert(r >= 0);
	}
}

static void cmd_delete(size_t begin, size_t end, const char * cmd)
{
	size_t i = begin;
	int r;

	r = line_goto(begin);
	if (r < 0)
	{
		fprintf(STDERR_FILENO, "Out of range lineno %u (file %u lines)\n", begin, ex_file.numlines);
		return;
	}

	while (i++ <= end)
		line_delete();
}

static void cmd_change(size_t begin, size_t end, const char * cmd)
{
	char * text;
	size_t i = begin;
	int r;

	r = line_goto(begin);
	if (r < 0)
	{
		fprintf(STDERR_FILENO, "Out of range lineno %u (file %u lines)\n", begin, ex_file.numlines);
		return;
	}

	while (i++ <= end)
		line_delete();

	// reposition for insert
	r = line_go(-1);
	assert(r >= 0);

	while (*(text = readline("")) != '.')
	{
		text = strdup(text);
		assert(text);
		r = line_append(text);
		assert(r >= 0);
	}
}

static void cmd_help(size_t begin, size_t end, const char * cmd)
{
	size_t i;

	printf("line numbers:\n  \"n\": n, +/-k: fwd/back k, \".\": current, \"$\": last in file, \"n,m\": [n,m]\n");

	for (i=0; i<CMDS_SIZE; i++)
	{
		if (cmds[i].f)
		{
			printf("%c - %s\n", CMD_BEGIN + i, cmds[i].description);
		}
	}
}


//
//

static void run_loop(void)
{
	while (1)
	{
		size_t begin = 1, end = 1;
		const char * l = readline(prompt);
		cmdfn f;
		int r;

		if (l[0] == 0)
		{
			cmd_set_lineno(ex_file.cur_lineno+1, ex_file.cur_lineno+1, l);
			continue;
		}

		r = parse_linenos(&l, &begin, &end);
		if (r < 0)
		{
			fprintf(STDOUT_FILENO, "lineno out of range\n");
			continue;
		}

		if (!*l)
		{
			cmd_set_lineno(begin, end, l);
			continue;
		}
		if (*l < CMD_BEGIN || CMD_END < *l)
		{
			printf("Command \"%s\" not implemented\n", l);
			continue;
		}
		f = cmds[CMDX(*l)].f;
		if (!f)
		{
			printf("Command \"%s\" not implemented\n", l);
			continue;
		}
		f(begin, end, l);
	}
}


//
// Startup 

#define COMMAND(c,_f,_d) do { cmds[CMDX(c)].f = _f; cmds[CMDX(c)].description = _d; } while(0)

static void register_commands(void)
{
	COMMAND('h', cmd_help, "show implemented commands");
	COMMAND('s', cmd_shell, "shell"); // this should be "sh", but 's' it is for now
	COMMAND('q', cmd_quit, "quit");
	COMMAND('w', cmd_write, "write");
	COMMAND('x', cmd_writequit, "write and quit");
	COMMAND('r', cmd_insert_file, "insert file");
	COMMAND('f', cmd_display_file, "display file information");
	COMMAND('p', cmd_display_lines, "display lines");
	COMMAND('#', cmd_display_lines_linenos, "display lines with linenos");
	COMMAND('=', cmd_get_lineno, "give current lineno");
	COMMAND('i', cmd_insert, "insert line");
	COMMAND('a', cmd_append, "append line");
	COMMAND('c', cmd_change, "change line");
	COMMAND('d', cmd_delete, "delete line");
	COMMAND('t', cmd_transfer, "transfer line");
	COMMAND('m', cmd_move, "move line");

/*
 * Maybe-implements:
 * - 'dk', delete k lines, and other commands using k
 * - Navigating around open files ('e', 'n', etc)
 * - '!', this just needs parsing code
 * - undo
 * - 'j', join, njm, j! to not insert space
 * - yank/paste, [x,y]y<a-zA-Z>, yank into buffer, cap to append
 * - search, support ^ and #, reverse search
 */
}

static void print_usage(const char * bin)
{
	fprintf(STDERR_FILENO, "%s [<file>]\n", bin);
}

static void parse_cmdline(int argc, char ** argv)
{
	char * bin = argv[0];
	char * file;
	int r;

	if (argc > 2 || get_arg_idx(argc, (const char **) argv, "-h"))
	{
		print_usage(bin);
		exit();
	}

	if (argc == 2)
	{
		file = argv[1];
		r = file_open(file, O_RDWR|O_CREAT);
		if (r < 0)
		{
			fprintf(STDERR_FILENO, "Unable to open \"%s\"\n", file);
			exit();
		}
	}
}

void umain(int argc, char ** argv)
{
	register_commands();
	ex_file_init(&ex_file);
	parse_cmdline(argc, argv);

	run_loop();
}
