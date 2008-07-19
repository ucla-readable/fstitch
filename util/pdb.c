/* This file is part of Featherstitch. Featherstitch is copyright 2005-2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <readline/readline.h>
#include <readline/history.h>

#include "lib/patchgroup_trace.h"

struct pgt_op {
	struct pgt_op * next;
	union {
		struct pgt_all all;
		struct pgt_create create;
		struct pgt_add_depend add_depend;
		struct pgt_release release;
		struct pgt_abandon abandon;
		struct pgt_label label;
	};
};

static int tty = 0;
static const char * input_name;

static int applied = 0;
static int op_count = 0;
static struct pgt_op * ops = NULL;
static struct pgt_op * current = NULL;

struct pg;

struct pg_dep {
	struct pg * pg;
	struct pg_dep * next;
};

struct pg_label {
	int count;
	char * label;
	struct pg_label * next;
};

struct pg {
	/* really the ID is part of the binding, but it can't be changed
	 * so all scopes will have the same ID and we store it here */
	patchgroup_id_t id;
	struct pg_dep * befores;
	struct pg_label * labels;
	int released;
	struct pg * next;
};

struct pg_binding {
	struct pg * pg;
	struct pg_binding * next;
};

struct pg_scope {
	pid_t pid;
	struct pg_binding * ids;
	struct pg_scope * next;
};

static struct pg * pgs = NULL;
static struct pg_scope * scopes = NULL;

static int read_header(FILE * input)
{
	struct pgt_header header;
	if(fread(&header, sizeof(header), 1, input) != 1)
		return -1;
	if(header.magic != PGT_MAGIC || header.version != PGT_VERSION)
		return -EPROTO;
	return 0;
}

static struct pgt_op * read_op(FILE * input)
{
	size_t size;
	struct pgt_op local;
	struct pgt_op * copy;
	if(fread(&local.all, sizeof(local.all), 1, input) != 1)
		return NULL;
	switch(local.all.type)
	{
		case PATCHGROUP_IOCTL_CREATE:
			size = sizeof(local.create);
			break;
		case PATCHGROUP_IOCTL_ADD_DEPEND:
			size = sizeof(local.add_depend);
			break;
		case PATCHGROUP_IOCTL_RELEASE:
			size = sizeof(local.release);
			break;
		case PATCHGROUP_IOCTL_ABANDON:
			size = sizeof(local.abandon);
			break;
		case -1:
			size = sizeof(local.label);
			break;
		default:
			return NULL;
	}
	if(fread(&(&local.all)[1], size - sizeof(local.all), 1, input) != 1)
		return NULL;
	if(local.all.type == -1)
		size += local.label.label_len + 1;
	copy = malloc(sizeof(local.next) + size);
	copy->next = NULL;
	if(local.all.type == -1)
	{
		memcpy(&copy->all, &local.all, size - local.label.label_len - 1);
		if(fread(&copy->label.label, local.label.label_len, 1, input) != 1)
		{
			free(copy);
			return NULL;
		}
		copy->label.label[local.label.label_len] = 0;
	}
	else
		memcpy(&copy->all, &local.all, size);
	return copy;
}

static int read_ops(FILE * input)
{
	int r, count = 0;
	struct pgt_op * last;
	struct pgt_op * next;
	r = read_header(input);
	if(r < 0)
		return r;
	for(last = ops; last && last->next; last = last->next);
	while((next = read_op(input)))
	{
		if(last)
			last->next = next;
		else
		{
			ops = next;
			current = next;
		}
		last = next;
		count++;
		if(last->all.pid != ops->all.pid)
			/* supporting multiple processes using the same
			 * patchgroups requires knowing when to fork the scopes,
			 * which the trace file doesn't tell us currently */
			break;
	}
	op_count += count;
	return next ? -ENOSYS : count;
}

static void free_ops(void)
{
	while(ops)
	{
		struct pgt_op * next = ops->next;
		free(ops);
		ops = next;
	}
	applied = 0;
	op_count = 0;
	current = NULL;
}

static void print_pgt_op(int number, struct pgt_op * op, FILE * output)
{
	fprintf(output, "#%d [%5d] @%u ", number, op->all.pid, (unsigned) (op->all.time - ops->all.time));
	switch(op->all.type)
	{
		case PATCHGROUP_IOCTL_CREATE:
			fprintf(output, "CREATE %d\n", op->create.id);
			break;
		case PATCHGROUP_IOCTL_ADD_DEPEND:
			fprintf(output, "ADD_DEPEND %d -> %d\n", op->add_depend.after, op->add_depend.before);
			break;
		case PATCHGROUP_IOCTL_RELEASE:
			fprintf(output, "RELEASE %d\n", op->release.id);
			break;
		case PATCHGROUP_IOCTL_ABANDON:
			fprintf(output, "ABANDON %d\n", op->abandon.id);
			break;
		case -1:
			fprintf(output, "LABEL %d \"%s\"\n", op->label.id, op->label.label);
			break;
		default:
			fprintf(output, "UNKNOWN\n");
	}
}

static int snprint_pgt_op(char * string, size_t length, struct pgt_op * op)
{
	switch(op->all.type)
	{
		case PATCHGROUP_IOCTL_CREATE:
			return snprintf(string, length, "CREATE %d", op->create.id);
		case PATCHGROUP_IOCTL_ADD_DEPEND:
			return snprintf(string, length, "ADD_DEPEND %d -> %d", op->add_depend.after, op->add_depend.before);
		case PATCHGROUP_IOCTL_RELEASE:
			return snprintf(string, length, "RELEASE %d", op->release.id);
		case PATCHGROUP_IOCTL_ABANDON:
			return snprintf(string, length, "ABANDON %d", op->abandon.id);
		case -1:
			return snprintf(string, length, "LABEL %d \"%s\"", op->label.id, op->label.label);
		default:
			return snprintf(string, length, "UNKNOWN");
	}
}

static struct pg_scope * lookup_scope(pid_t pid)
{
	struct pg_scope * scope;
	for(scope = scopes; scope; scope = scope->next)
		if(scope->pid == pid)
			return scope;
	return NULL;
}

static struct pg * lookup_pg(struct pg_scope * scope, patchgroup_id_t id)
{
	struct pg_binding * binding;
	for(binding = scope->ids; binding; binding = binding->next)
		if(binding->pg->id == id)
			return binding->pg;
	return NULL;
}

static int apply_op(struct pgt_op * op)
{
	struct pg_scope * scope = lookup_scope(op->all.pid);
	switch(op->all.type)
	{
		case PATCHGROUP_IOCTL_CREATE:
		{
			struct pg_binding * binding;
			if(!scope)
			{
				scope = malloc(sizeof(*scope));
				scope->pid = op->all.pid;
				scope->ids = NULL;
				scope->next = scopes;
				scopes = scope;
			}
			binding = malloc(sizeof(*binding));
			binding->pg = malloc(sizeof(*binding->pg));
			binding->pg->id = op->create.id;
			binding->pg->befores = NULL;
			binding->pg->labels = NULL;
			binding->pg->released = 0;
			binding->pg->next = pgs;
			pgs = binding->pg;
			binding->next = scope->ids;
			scope->ids = binding;
			break;
		}
		case PATCHGROUP_IOCTL_ADD_DEPEND:
		{
			struct pg * after = lookup_pg(scope, op->add_depend.after);
			struct pg * before = lookup_pg(scope, op->add_depend.before);
			struct pg_dep * dep = malloc(sizeof(*dep));
			dep->pg = before;
			dep->next = after->befores;
			after->befores = dep;
			break;
		}
		case PATCHGROUP_IOCTL_RELEASE:
		{
			struct pg * pg = lookup_pg(scope, op->release.id);
			pg->released = 1;
			break;
		}
		case PATCHGROUP_IOCTL_ABANDON:
		{
			struct pg_binding ** next = &scope->ids;
			while(*next && (*next)->pg->id != op->abandon.id)
				next = &(*next)->next;
			if(*next)
			{
				struct pg_binding * old = *next;
				*next = old->next;
				free(old);
			}
			break;
		}
		case -1:
		{
			struct pg * pg = lookup_pg(scope, op->label.id);
			struct pg_label * label;
			for(label = pg->labels; label; label = label->next)
				if(!strcmp(label->label, op->label.label))
					break;
			if(label)
				label->count++;
			else
			{
				label = malloc(sizeof(*label));
				label->count = 1;
				/* don't need to copy it */
				label->label = op->label.label;
				label->next = pg->labels;
				pg->labels = label;
			}
			break;
		}
		default:
			return -1;
	}
	return 0;
}

static int apply_current(void)
{
	int r;
	if(!current)
		return 0;
	r = apply_op(current);
	if(r < 0)
		return r;
	current = current->next;
	applied++;
	return 0;
}

static void reset_state(void)
{
	applied = 0;
	current = ops;
	while(pgs)
	{
		struct pg * old = pgs;
		pgs = pgs->next;
		while(old->befores)
		{
			struct pg_dep * next = old->befores->next;
			free(old->befores);
			old->befores = next;
		}
		while(old->labels)
		{
			struct pg_label * next = old->labels->next;
			free(old->labels);
			old->labels = next;
		}
	}
	while(scopes)
	{
		struct pg_scope * old = scopes;
		scopes = scopes->next;
		while(old->ids)
		{
			struct pg_binding * next = old->ids->next;
			free(old->ids);
			old->ids = next;
		}
	}
}

static void render_pg(FILE * output, struct pg * pg)
{
	struct pg_dep * dep;
	struct pg_label * label;
	fprintf(output, "\"pg%d-%p\" [label=\"ID %d", pg->id, pg, pg->id);
	for(label = pg->labels; label; label = label->next)
		if(label->count > 1)
			fprintf(output, "\\n\\\"%s\\\" (x%d)", label->label, label->count);
		else
			fprintf(output, "\\n\\\"%s\\\"", label->label);
	fprintf(output, "\",fillcolor=lightgray,style=\"filled");
	if(!pg->released)
		fprintf(output, ",dashed,bold");
	fprintf(output, "\"]\n");
	for(dep = pg->befores; dep; dep = dep->next)
		fprintf(output, "\"pg%d-%p\" -> \"pg%d-%p\" [color=black]\n", pg->id, pg, dep->pg->id, dep->pg);
}

static void render(FILE * output, const char * title, int landscape)
{
	struct pg * pg;
	
	/* header */
	fprintf(output, "digraph \"debug: %d/%d patchgroup operation%s, %s\"\n", applied, op_count, (op_count == 1) ? "" : "s", input_name);
	fprintf(output, "{\nnodesep=0.25;\nranksep=0.25;\nfontname=\"Helvetica\";\nfontsize=10;\n");
	if(landscape)
		fprintf(output, "rankdir=LR;\norientation=L;\nsize=\"10,7.5\";\n");
	else
		fprintf(output, "rankdir=LR;\norientation=P;\nsize=\"16,16\";\n");
	fprintf(output, "subgraph clusterAll {\nlabel=\"%s\";\ncolor=white;\n", title);
	fprintf(output, "node [shape=ellipse,color=black,fontname=\"Helvetica\",fontsize=10];\n");
	
	for(pg = pgs; pg; pg = pg->next)
		render_pg(output, pg);
	
	/* footer */
	fprintf(output, "}\n}\n");
}

static int command_jump(int argc, const char * argv[])
{
	int target;
	int progress = 0, distance, percent = -1;
	if(argc < 2)
	{
		printf("Need a patchgroup operation to jump to.");
		return -1;
	}
	target = atoi(argv[1]);
	if(target < 0 || target > op_count)
	{
		printf("No such patchgroup operation.\n");
		return -1;
	}
	printf("Replaying log... %s", tty ? "    " : "");
	fflush(stdout);
	if(target < applied)
		reset_state();
	distance = target - applied;
	while(applied < target)
	{
		int r;
		if(tty)
		{
			int p = progress * 100 / distance;
			if(p > percent)
			{
				percent = p;
				printf("\e[4D%2d%% ", percent);
				fflush(stdout);
			}
		}
		r = apply_current();
		if(r < 0)
		{
			printf("error %d applying patchgroup operation %d (%s)\n", -r, applied + 1, strerror(-r));
			return r;
		}
		progress++;
	}
	printf("%s%d patchgroup operation%s OK!\n", tty ? "\e[4D" : "", applied, (applied == 1) ? "" : "s");
	return 0;
}

static int command_list(int argc, const char * argv[])
{
	struct pgt_op * op;
	int i, min = 0, max = op_count - 1;
	const char * filename = NULL;
	FILE * output = stdout;
	if(argc > 1)
	{
		if(!strcmp(argv[argc - 2], ">"))
		{
			filename = argv[argc - 1];
			argc -= 2;
		}
		else if(argv[argc - 1][0] == '>')
			filename = &argv[--argc][1];
		if(filename)
		{
			output = fopen(filename, "w");
			if(!output)
			{
				printf("Error opening %s.\n", filename);
				return -1;
			}
		}
	}
	if(argc == 2)
	{
		/* show a single operation */
		min = max = atoi(argv[1]) - 1;
		if(min < 0 || max >= op_count)
		{
			printf("No such patchgroup operation.\n");
			goto error_file;
		}
	}
	else if(argc > 2)
	{
		/* show an operation range */
		min = atoi(argv[1]) - 1;
		max = atoi(argv[2]) - 1;
		if(min < 0 || min > max)
		{
			printf("Invalid range.\n");
			goto error_file;
		}
		if(max >= op_count)
			max = op_count - 1;
	}
	if(min < applied)
	{
		op = ops;
		i = 0;
	}
	else
	{
		op = current;
		i = applied;
	}
	for(; i < min; i++)
		op = op->next;
	for(i = min; i <= max; i++)
	{
		print_pgt_op(i + 1, op, output);
		op = op->next;
	}
	if(filename)
		fclose(output);
	return 0;
	
error_file:
	if(filename)
	{
		fclose(output);
		unlink(filename);
	}
	return -1;
}

static int command_render(int argc, const char * argv[])
{
	FILE * output = stdout;
	char title[256] = {0};
	if(argc > 1)
	{
		output = fopen(argv[1], "w");
		if(!output)
		{
			perror(argv[1]);
			return -errno;
		}
	}
	if(applied > 0)
	{
		struct pgt_op * prev;
		/* this is kind of unfortunate */
		for(prev = ops; prev->next != current; prev = prev->next);
		snprint_pgt_op(title, sizeof(title), prev);
	}
	render(output, title, 1);
	if(argc > 1)
		fclose(output);
	return 0;
}

static int command_run(int argc, const char * argv[])
{
	char number[12];
	const char * array[] = {"jump", number};
	sprintf(number, "%u", op_count);
	return command_jump(2, array);
}

static int command_reset(int argc, const char * argv[])
{
	reset_state();
	return 0;
}

static int command_status(int argc, const char * argv[])
{
	printf("Debugging %s, read %d patchgroup operation%s, applied %d\n", input_name, op_count, (op_count == 1) ? "" : "s", applied);
	return 0;
}

static int command_step(int argc, const char * argv[])
{
	int delta = (argc > 1) ? atoi(argv[1]) : 1;
	int target = applied + delta;
	if(target < 0 || target > op_count)
	{
		printf("No such patchgroup operation.\n");
		return -1;
	}
	printf("Replaying log... ");
	fflush(stdout);
	if(target < applied)
		reset_state();
	while(applied < target)
	{
		int r = apply_current();
		if(r < 0)
		{
			printf("error %d applying patchgroup operation %d (%s)\n", -r, applied + 1, strerror(-r));
			return r;
		}
	}
	printf("%d patchgroup operation%s OK!\n", applied, (applied == 1) ? "" : "s");
	return 0;
}

static int command_help(int argc, const char * argv[]);
static int command_quit(int argc, const char * argv[]);

struct {
	const char * command;
	const char * help;
	int (*execute)(int argc, const char * argv[]);
} commands[] = {
	{"jump", "Jump to a specified position.", command_jump},
	{"list", "List operations in a specified range.", command_list},
	{"reset", "Reset to beginning of trace.", command_reset},
	{"render", "Render to a GraphViz dot file.", command_render},
	{"run", "Run entire patchgroup trace.", command_run},
	{"status", "Displays system state status.", command_status},
	{"step", "Step a specified number of operations.", command_step},
	{"help", "Displays help.", command_help},
	{"quit", "Quits the program.", command_quit}
};
#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static int command_help(int argc, const char * argv[])
{
	int i;
	if(argc < 2)
	{
		printf("Commands:\n");
		for(i = 0; i < COMMAND_COUNT; i++)
			printf("  %s\n    %s\n", commands[i].command, commands[i].help);
	}
	else
		for(i = 0; i < COMMAND_COUNT; i++)
		{
			if(strcmp(commands[i].command, argv[1]))
				continue;
			printf("  %s\n    %s\n", commands[i].command, commands[i].help);
			break;
		}
	return 0;
}

static int command_quit(int argc, const char * argv[])
{
	return -EINTR;
}

static int command_line_execute(char * line)
{
	int i, argc = 0;
	const char * argv[64];
	do {
		while(*line == ' ' || *line == '\n')
			line++;
		if(!*line)
			break;
		argv[argc++] = line;
		while(*line && *line != ' ' && *line != '\n')
			line++;
		if(*line)
			*(line++) = 0;
		else
			break;
	} while(argc < 64);
	if(*line)
		return -E2BIG;
	if(!argc)
		return 0;
	for(i = 0; i < COMMAND_COUNT; i++)
		if(!strcmp(commands[i].command, argv[0]))
			return commands[i].execute(argc, argv);
	return -ENOENT;
}

int main(int argc, char * argv[])
{
	int count;
	char * line;
	FILE * input;
	tty = isatty(1);
	if(argc < 1)
	{
		printf("Usage: %s <trace>\n", argv[0]);
		return 0;
	}
	input_name = argv[1];
	input = fopen(input_name, "r");
	if(!input)
	{
		perror(input_name);
		return 1;
	}
	count = read_ops(input);
	fclose(input);
	if(count < 0)
	{
		printf("Error reading file.\n");
		return 1;
	}
	printf("Read %d patchgroup operations.\n", count);
	while((line = readline("pdb> ")))
	{
		int r;
		for(r = 0; line[r] == ' '; r++);
		if(line[r])
			add_history(line);
		r = command_line_execute(line);
		if(r == -E2BIG)
			printf("Too many tokens on command line!\n");
		else if(r == -ENOENT)
			printf("No such command.\n");
		else if(r == -EINTR)
			break;
		free(line);
	}
	if(line)
		free(line);
	else
		printf("\n");
	reset_state();
	free_ops();
	return 0;
}
