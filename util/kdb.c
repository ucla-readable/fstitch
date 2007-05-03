#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <readline/readline.h>
#include <readline/history.h>

#define WANT_DEBUG_STRUCTURES 1
#include "kfs/debug_opcode.h"

/* Set HASH_PRIME to do an extra pass over the input file to prime the string
 * and stack hash tables, and to report on the result. */
#define HASH_PRIME 0

/* Set RANDOM_TEST to do a sequence of random opcode reads after loading an
 * input file to test the speed of non-sequential access. */
#define RANDOM_TEST 0

/* Begin unique immutable strings */

#define HASH_TABLE_SIZE 65521

static uint32_t strsum(const char * string)
{
	uint32_t sum = 0x5AFEDA7A;
	while(*string)
	{
		/* ROL 3 */
		sum = (sum << 3) | (sum >> 29);
		sum ^= *(string++);
	}
	return sum;
}

static int unique_strings = 0;

static const char * strdup_unique(char * string)
{
	struct hash_entry {
		const char * string;
		uint32_t sum;
		struct hash_entry * next;
	};
	static struct hash_entry * hash_table[HASH_TABLE_SIZE];
	uint32_t sum = strsum(string);
	uint32_t index = sum % HASH_TABLE_SIZE;
	struct hash_entry * scan;
	for(scan = hash_table[index]; scan; scan = scan->next)
		if(scan->sum == sum && !strcmp(scan->string, string))
			break;
	if(!scan)
	{
		scan = malloc(sizeof(*scan));
		if(!scan)
			return NULL;
		scan->string = strdup(string);
		scan->sum = sum;
		scan->next = hash_table[index];
		hash_table[index] = scan;
		unique_strings++;
	}
	return scan->string;
}

/* End unique immutable strings */

/* Begin unique immutable stacks */

static uint32_t stksum(const uint32_t * stack)
{
	uint32_t sum = 0x5AFEDA7A;
	while(*stack)
	{
		/* ROL 3 */
		sum = (sum << 3) | (sum >> 29);
		sum ^= *(stack++);
	}
	return sum;
}

static int stkcmp(const uint32_t * s1, const uint32_t * s2)
{
	int i;
	for(i = 0; s1[i] && s1[i] == s2[i]; i++);
	return s1[i] - s2[i];
}

static uint32_t * stkdup(const uint32_t * stack)
{
	int length;
	uint32_t * copy;
	for(length = 0; stack[length]; length++);
	copy = malloc(++length * sizeof(*copy));
	if(!copy)
		return NULL;
	memcpy(copy, stack, length * sizeof(copy));
	return copy;
}

static int unique_stacks = 0;

static const uint32_t * stkdup_unique(uint32_t * stack)
{
	struct hash_entry {
		const uint32_t * stack;
		uint32_t sum;
		struct hash_entry * next;
	};
	static struct hash_entry * hash_table[HASH_TABLE_SIZE];
	uint32_t sum = stksum(stack);
	uint32_t index = sum % HASH_TABLE_SIZE;
	struct hash_entry * scan;
	for(scan = hash_table[index]; scan; scan = scan->next)
		if(scan->sum == sum && !stkcmp(scan->stack, stack))
			break;
	if(!scan)
	{
		scan = malloc(sizeof(*scan));
		if(!scan)
			return NULL;
		scan->stack = stkdup(stack);
		scan->sum = sum;
		scan->next = hash_table[index];
		hash_table[index] = scan;
		unique_stacks++;
	}
	return scan->stack;
}

/* End unique immutable stacks */

/* Begin opcode reading */

struct opcode_offsets {
	int size, full;
	off_t * offsets;
	struct opcode_offsets * next;
};

static int opcodes = 0;
static struct opcode_offsets * offsets = NULL;
static struct opcode_offsets * last_offsets = NULL;

static int add_opcode_offset(off_t offset)
{
	if(!last_offsets || last_offsets->full == last_offsets->size)
	{
		struct opcode_offsets * extra = malloc(sizeof(*extra));
		if(!extra)
			return -ENOMEM;
		if(!last_offsets)
			extra->size = 64;
		else if(last_offsets->size < 32768)
			extra->size = last_offsets->size * 2;
		else
			extra->size = 32768;
		extra->full = 0;
		extra->offsets = calloc(extra->size, sizeof(*extra->offsets));
		if(!extra->offsets)
		{
			free(extra);
			return -ENOMEM;
		}
		extra->next = NULL;
		if(last_offsets)
			last_offsets->next = extra;
		else
			offsets = extra;
		last_offsets = extra;
	}
	last_offsets->offsets[last_offsets->full++] = offset;
	return ++opcodes;
}

static off_t get_opcode_offset(int index)
{
	static struct opcode_offsets * scan = NULL;
	static int scan_index = 0;
	if(scan && scan_index < index)
		index -= scan_index;
	else
	{
		scan = offsets;
		scan_index = 0;
	}
	for(; scan && index >= scan->size; scan = scan->next)
	{
		index -= scan->size;
		scan_index += scan->size;
	}
	return scan ? scan->offsets[index] : -EINVAL;
}

#ifdef __linux__
#define fgetc fgetc_unlocked
#define feof feof_unlocked
#endif

static FILE * input = NULL;
static const char * input_name = NULL;

static int read_lit_8(uint8_t * data)
{
	*data = fgetc(input);
	if(feof(input))
		return -1;
	return 0;
}

static int read_lit_16(uint16_t * data)
{
	*data = fgetc(input);
	if(feof(input))
		return -1;
	*data <<= 8;
	*data |= fgetc(input);
	if(feof(input))
		return -1;
	return 0;
}

/* This function is called an order of magnitude more frequently than any other
 * function. It is important that it be very fast. So, unroll the obvious loop. */
static int read_lit_32(uint32_t * data)
{
	*data = fgetc(input);
	if(feof(input))
		return -1;
	*data <<= 8;
	*data |= fgetc(input);
	if(feof(input))
		return -1;
	*data <<= 8;
	*data |= fgetc(input);
	if(feof(input))
		return -1;
	*data <<= 8;
	*data |= fgetc(input);
	if(feof(input))
		return -1;
	return 0;
}

static int read_lit_str(const char ** data, int allocate)
{
	int i;
	static char buffer[128];
	for(i = 0; i < sizeof(buffer); i++)
	{
		buffer[i] = fgetc(input);
		if(feof(input))
			return -1;
		if(!buffer[i])
			break;
	}
	*data = allocate ? strdup_unique(buffer) : buffer;
	return 0;
}

static int read_debug_signature(void)
{
	int m, o, r;
	uint16_t zero;
	uint32_t debug_rev, debug_opcode_rev;
	
	r = read_lit_32(&debug_rev);
	if(r < 0)
		return r;
	r = read_lit_32(&debug_opcode_rev);
	if(r < 0)
		return r;
	if(debug_rev != 3379 || debug_opcode_rev != 2934)
		return -EPROTO;
	
	for(m = 0; modules[m].opcodes; m++)
		for(o = 0; modules[m].opcodes[o]->params; o++)
		{
			int p;
			uint8_t zero;
			const char * o_name;
			uint16_t module, opcode;
			r = read_lit_16(&module);
			if(r < 0)
				return r;
			if(module != modules[m].module)
				return -EPROTO;
			r = read_lit_16(&opcode);
			if(r < 0)
				return r;
			if(opcode != modules[m].opcodes[o]->opcode)
				return -EPROTO;
			r = read_lit_str(&o_name, 0);
			if(r < 0)
				return r;
			if(strcmp(o_name, modules[m].opcodes[o]->name))
				return -EPROTO;
			for(p = 0; modules[m].opcodes[o]->params[p]->name; p++)
			{
				uint8_t size;
				const char * p_name;
				r = read_lit_8(&size);
				if(r < 0)
					return r;
				if(size != type_sizes[modules[m].opcodes[o]->params[p]->type])
					return -EPROTO;
				r = read_lit_str(&p_name, 0);
				if(r < 0)
					return r;
				if(strcmp(p_name, modules[m].opcodes[o]->params[p]->name))
					return -EPROTO;
			}
			r = read_lit_8(&zero);
			if(r < 0)
				return r;
			if(zero)
				return -EPROTO;
		}
	r = read_lit_16(&zero);
	if(r < 0)
		return r;
	if(zero)
		return -EPROTO;
	
	return 0;
}

struct debug_param {
	union {
		uint8_t size;
		/* name is only used as input to param_lookup */
		const char * name;
	};
	union {
		uint32_t data_4;
		uint16_t data_2;
		uint8_t data_1;
		const char * data_v;
	};
};

static int scan_opcode(void)
{
	int m, o = 0, p, r;
	const char * file;
	uint32_t line;
	const char * function;
	uint16_t module, opcode, zero;
	uint32_t stack;
	r = read_lit_str(&file, 0);
	if(r < 0)
		return r;
	r = read_lit_32(&line);
	if(r < 0)
		return r;
	r = read_lit_str(&function, 0);
	if(r < 0)
		return r;
	r = read_lit_16(&module);
	if(r < 0)
		return r;
	r = read_lit_16(&opcode);
	if(r < 0)
		return r;
	for(m = 0; modules[m].opcodes; m++)
		if(modules[m].module == module)
		{
			for(o = 0; modules[m].opcodes[o]->params; o++)
				if(modules[m].opcodes[o]->opcode == opcode)
					break;
			break;
		}
	if(!modules[m].opcodes || !modules[m].opcodes[o]->params)
		return -EPROTO;
	for(p = 0; r >= 0 && modules[m].opcodes[o]->params[p]->name; p++)
	{
		struct debug_param param;
		r = read_lit_8(&param.size);
		if(r < 0)
			return r;
		if(param.size != type_sizes[modules[m].opcodes[o]->params[p]->type])
			return -EPROTO;
		if(param.size == 4)
			r = read_lit_32(&param.data_4);
		else if(param.size == 2)
			r = read_lit_16(&param.data_2);
		else if(param.size == 1)
			r = read_lit_8(&param.data_1);
		else if(param.size == (uint8_t) -1)
			r = read_lit_str(&param.data_v, 0);
		if(r < 0)
			return r;
	}
	r = read_lit_16(&zero);
	if(r < 0)
		return r;
	if(zero)
		return -EPROTO;
	do {
		r = read_lit_32(&stack);
		if(r < 0)
			return r;
	} while(stack);
	return 0;
}

struct debug_opcode {
	const char * file;
	uint32_t line;
	const char * function;
	int module_idx, opcode_idx;
	const uint32_t * stack;
	struct debug_param * params;
};

static int read_opcode(struct debug_opcode * debug_opcode)
{
	int i, m, o = 0, p, r;
	uint16_t module, opcode, zero;
	uint32_t stack[128];
	r = read_lit_str(&debug_opcode->file, 1);
	if(r < 0)
		return r;
	r = read_lit_32(&debug_opcode->line);
	if(r < 0)
		return r;
	r = read_lit_str(&debug_opcode->function, 1);
	if(r < 0)
		return r;
	r = read_lit_16(&module);
	if(r < 0)
		return r;
	r = read_lit_16(&opcode);
	if(r < 0)
		return r;
	for(m = 0; modules[m].opcodes; m++)
		if(modules[m].module == module)
		{
			for(o = 0; modules[m].opcodes[o]->params; o++)
				if(modules[m].opcodes[o]->opcode == opcode)
					break;
			break;
		}
	if(!modules[m].opcodes || !modules[m].opcodes[o]->params)
		return -EPROTO;
	debug_opcode->module_idx = m;
	debug_opcode->opcode_idx = o;
	for(p = 0; modules[m].opcodes[o]->params[p]->name; p++);
	debug_opcode->params = malloc(p * sizeof(*debug_opcode->params));
	if(!debug_opcode->params)
		return -ENOMEM;
	i = 0;
	for(p = 0; r >= 0 && modules[m].opcodes[o]->params[p]->name; p++)
	{
		struct debug_param * param = &debug_opcode->params[i++];
		r = read_lit_8(&param->size);
		if(r < 0)
			goto error_params;
		if(param->size != type_sizes[modules[m].opcodes[o]->params[p]->type])
		{
			r = -EPROTO;
			goto error_params;
		}
		if(param->size == 4)
			r = read_lit_32(&param->data_4);
		else if(param->size == 2)
			r = read_lit_16(&param->data_2);
		else if(param->size == 1)
			r = read_lit_8(&param->data_1);
		else if(param->size == (uint8_t) -1)
			r = read_lit_str(&param->data_v, 1);
		if(r < 0)
			goto error_params;
	}
	r = read_lit_16(&zero);
	if(r < 0)
		goto error_params;
	if(zero)
	{
		r = -EPROTO;
		goto error_params;
	}
	i = 0;
	do {
		if(i == 128)
		{
			r = -E2BIG;
			goto error_params;
		}
		r = read_lit_32(&stack[i]);
		if(r < 0)
			goto error_params;
	} while(stack[i++]);
	debug_opcode->stack = stkdup_unique(stack);
	return 0;

error_params:
	free(debug_opcode->params);
	return r;
}

static int get_opcode(int index, struct debug_opcode * debug_opcode)
{
	static int last_index = -1;
	if(last_index == -1 || index != last_index + 1)
		fseeko(input, get_opcode_offset(index), SEEK_SET);
	last_index = index;
	return read_opcode(debug_opcode);
}

static void put_opcode(struct debug_opcode * debug_opcode)
{
	free(debug_opcode->params);
}

/* End opcode reading */

/* Begin state management */

struct bd {
	uint32_t address;
	const char * name;
	struct bd * next;
};

struct block {
	uint32_t address;
	uint32_t number;
	struct block * next;
};

struct arrow {
	uint32_t chdesc;
	struct arrow * next;
};

struct label {
	const char * label;
	struct label * next;
};

struct chdesc {
	uint32_t address;
	int opcode;
	uint32_t owner, block;
	enum {BIT, BYTE, NOOP} type;
	union {
		struct {
			uint16_t offset;
			uint32_t xor;
		} bit;
		struct {
			uint16_t offset, length;
		} byte;
	};
	uint16_t flags;
	struct arrow * befores;
	struct arrow * afters;
	struct label * labels;
	struct chdesc * next;
};

static struct bd * bds = NULL;
static struct block * blocks = NULL;
static struct chdesc * chdescs = NULL;
static int chdesc_count = 0;
static int arrow_count = 0;

static int applied = 0;

static void free_arrows(struct arrow ** point)
{
	while(*point)
	{
		struct arrow * old = *point;
		*point = old->next;
		free(old);
		arrow_count--;
	}
}

static void free_labels(struct label ** point)
{
	while(*point)
	{
		struct label * old = *point;
		*point = old->next;
		free(old);
	}
}

static void reset_state(void)
{
	while(bds)
	{
		struct bd * old = bds;
		bds = old->next;
		free(old);
	}
	while(blocks)
	{
		struct block * old = blocks;
		blocks = old->next;
		free(old);
	}
	while(chdescs)
	{
		struct chdesc * old = chdescs;
		chdescs = old->next;
		free_arrows(&old->befores);
		free_arrows(&old->afters);
		free_labels(&old->labels);
		free(old);
	}
	chdesc_count = 0;
	arrow_count = 0;
	applied = 0;
}

static struct bd * lookup_bd(uint32_t address)
{
	struct bd * scan;
	for(scan = bds; scan; scan = scan->next)
		if(scan->address == address)
			break;
	return scan;
}

static struct block * lookup_block(uint32_t address)
{
	struct block * scan;
	for(scan = blocks; scan; scan = scan->next)
		if(scan->address == address)
			break;
	return scan;
}

static struct chdesc * lookup_chdesc(uint32_t address)
{
	struct chdesc * scan;
	for(scan = chdescs; scan; scan = scan->next)
		if(scan->address == address)
			break;
	return scan;
}

static int add_bd_name(uint32_t address, const char * name)
{
	struct bd * bd = lookup_bd(address);
	if(bd)
	{
		bd->name = name;
		return 0;
	}
	bd = malloc(sizeof(*bd));
	if(!bd)
		return -ENOMEM;
	bd->address = address;
	bd->name = name;
	bd->next = bds;
	bds = bd;
	return 0;
}

static int add_block_number(uint32_t address, uint32_t number)
{
	struct block * block = lookup_block(address);
	if(block)
	{
		block->number = number;
		return 0;
	}
	block = malloc(sizeof(*block));
	if(!block)
		return -ENOMEM;
	block->address = address;
	block->number = number;
	block->next = blocks;
	blocks = block;
	return 0;
}

static struct chdesc * _chdesc_create(uint32_t address, uint32_t owner)
{
	struct chdesc * chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	chdesc->address = address;
	chdesc->opcode = applied + 1;
	chdesc->owner = owner;
	chdesc->flags = 0;
	chdesc->befores = NULL;
	chdesc->afters = NULL;
	chdesc->next = chdescs;
	chdescs = chdesc;
	chdesc_count++;
	return chdesc;
}

static struct chdesc * chdesc_create_bit(uint32_t address, uint32_t owner, uint32_t block, uint16_t offset, uint32_t xor)
{
	struct chdesc * chdesc = _chdesc_create(address, owner);
	if(!chdesc)
		return NULL;
	chdesc->block = block;
	chdesc->type = BIT;
	chdesc->bit.offset = offset;
	chdesc->bit.xor = xor;
	return chdesc;
}

static struct chdesc * chdesc_create_byte(uint32_t address, uint32_t owner, uint32_t block, uint16_t offset, uint16_t length)
{
	struct chdesc * chdesc = _chdesc_create(address, owner);
	if(!chdesc)
		return NULL;
	chdesc->block = block;
	chdesc->type = BYTE;
	chdesc->byte.offset = offset;
	chdesc->byte.length = length;
	return chdesc;
}

static struct chdesc * chdesc_create_noop(uint32_t address, uint32_t owner)
{
	struct chdesc * chdesc = _chdesc_create(address, owner);
	if(!chdesc)
		return NULL;
	chdesc->block = 0;
	chdesc->type = NOOP;
	return chdesc;
}

static int chdesc_add_label(struct chdesc * chdesc, const char * label)
{
	struct label * scan;
	for(scan = chdesc->labels; scan; scan = scan->next)
		if(!strcmp(scan->label, label))
			return 0;
	scan = malloc(sizeof(*scan));
	if(!scan)
		return -ENOMEM;
	scan->label = label;
	scan->next = chdesc->labels;
	chdesc->labels = scan;
	return 0;
}

static int chdesc_add_before(struct chdesc * after, uint32_t before)
{
	struct arrow * arrow = malloc(sizeof(*arrow));
	if(!arrow)
		return -ENOMEM;
	arrow->chdesc = before;
	arrow->next = after->befores;
	after->befores = arrow;
	arrow_count++;
	return 0;
}

static int chdesc_add_after(uint32_t after, struct chdesc * before)
{
	struct arrow * arrow = malloc(sizeof(*arrow));
	if(!arrow)
		return -ENOMEM;
	arrow->chdesc = after;
	arrow->next = before->afters;
	before->afters = arrow;
	arrow_count++;
	return 0;
}

static int chdesc_rem_before(struct chdesc * after, uint32_t before)
{
	struct arrow ** point;
	for(point = &after->befores; *point; point = &(*point)->next)
		if((*point)->chdesc == before)
		{
			struct arrow * old = *point;
			*point = old->next;
			free(old);
			arrow_count--;
			return 0;
		}
	return -ENOENT;
}

static int chdesc_rem_after(uint32_t after, struct chdesc * before)
{
	struct arrow ** point;
	for(point = &before->afters; *point; point = &(*point)->next)
		if((*point)->chdesc == after)
		{
			struct arrow * old = *point;
			*point = old->next;
			free(old);
			arrow_count--;
			return 0;
		}
	return -ENOENT;
}

static int chdesc_destroy(uint32_t address)
{
	struct chdesc ** point = &chdescs;
	for(point = &chdescs; *point; point = &(*point)->next)
		if((*point)->address == address)
		{
			struct chdesc * old = *point;
			*point = old->next;
			free_arrows(&old->befores);
			free_arrows(&old->afters);
			free_labels(&old->labels);
			chdesc_count--;
			return 0;
		}
	return -ENOENT;
}

/* End state management */

/* Begin commands */

static int param_lookup(struct debug_opcode * opcode, struct debug_param * table)
{
	int m = opcode->module_idx;
	int o = opcode->opcode_idx;
	int i, j = 0;
	for(i = 0; table[i].name; i++)
	{
		for(; modules[m].opcodes[o]->params[j]->name; j++)
			if(!strcmp(modules[m].opcodes[o]->params[j]->name, table[i].name))
				break;
		if(!modules[m].opcodes[o]->params[j]->name)
		{
			for(j = 0; modules[m].opcodes[o]->params[j]->name; j++)
				if(!strcmp(modules[m].opcodes[o]->params[j]->name, table[i].name))
					break;
			if(!modules[m].opcodes[o]->params[j]->name)
				return -ENOENT;
		}
		table[i] = opcode->params[j++];
	}
	return 0;
}

static int command_jump(int argc, const char * argv[])
{
	int target, progress = 0, distance, percent = -1;
	if(argc < 2)
	{
		printf("Need an opcode to jump to.");
		return -1;
	}
	target = atoi(argv[1]);
	printf("[Jump: %d to %d]\n", applied, target);
	printf("Replaying log...     ");
	fflush(stdout);
	if(target < applied)
		reset_state();
	distance = target - applied;
	while(applied < target)
	{
		int m, o, p, r;
		struct debug_opcode opcode;
		p = progress * 100 / distance;
		if(p > percent)
		{
			percent = p;
			printf("\e[4D%2d%% ", percent);
			fflush(stdout);
		}
		r = get_opcode(applied, &opcode);
		if(r < 0)
		{
			printf("error %d reading opcode %d (%s)\n", -r, applied + 1, strerror(-r));
			return r;
		}
		m = opcode.module_idx;
		o = opcode.opcode_idx;
		switch(modules[m].opcodes[o]->opcode)
		{
			case KDB_INFO_MARK:
				/* nothing */
				break;
			case KDB_INFO_BD_NAME:
			{
				struct debug_param params[] = {
					{{.name = "bd"}},
					{{.name = "name"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					goto fail;
				assert(params[0].size == 4 && params[1].size == (uint8_t) -1);
				r = add_bd_name(params[0].data_4, params[1].data_v);
				if(r < 0)
					goto fail;
				break;
			}
			case KDB_INFO_BDESC_NUMBER:
			{
				struct debug_param params[] = {
					{{.name = "block"}},
					{{.name = "number"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					goto fail;
				assert(params[0].size == 4 && params[1].size == 4);
				r = add_block_number(params[0].data_4, params[1].data_4);
				if(r < 0)
					goto fail;
				break;
			}
			case KDB_INFO_CHDESC_LABEL:
			{
				struct chdesc * chdesc;
				struct debug_param params[] = {
					{{.name = "chdesc"}},
					{{.name = "label"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					goto fail;
				assert(params[0].size == 4 && params[1].size == (uint8_t) -1);
				chdesc = lookup_chdesc(params[0].data_4);
				if(!chdesc)
				{
					r = -EFAULT;
					goto fail;
				}
				r = chdesc_add_label(chdesc, params[1].data_v);
				if(r < 0)
					goto fail;
				break;
			}
			
			case KDB_BDESC_ALLOC:
			case KDB_BDESC_ALLOC_WRAP:
			case KDB_BDESC_RETAIN:
			case KDB_BDESC_RELEASE:
			case KDB_BDESC_DESTROY:
			case KDB_BDESC_FREE_DDESC:
			case KDB_BDESC_AUTORELEASE:
			case KDB_BDESC_AR_RESET:
			case KDB_BDESC_AR_POOL_PUSH:
			case KDB_BDESC_AR_POOL_POP:
				/* unsupported */
				break;
			
			case KDB_CHDESC_CREATE_NOOP:
			{
				struct debug_param params[] = {
					{{.name = "chdesc"}},
					{{.name = "owner"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					goto fail;
				assert(params[0].size == 4 && params[1].size == 4);
				if(!chdesc_create_noop(params[0].data_4, params[1].data_4))
				{
					r = -ENOMEM;
					goto fail;
				}
				break;
			}
			case KDB_CHDESC_CREATE_BIT:
			{
				struct debug_param params[] = {
					{{.name = "chdesc"}},
					{{.name = "block"}},
					{{.name = "owner"}},
					{{.name = "offset"}},
					{{.name = "xor"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					goto fail;
				assert(params[0].size == 4 && params[1].size == 4 &&
				       params[2].size == 4 && params[3].size == 2 &&
				       params[4].size == 4);
				if(!chdesc_create_bit(params[0].data_4, params[2].data_4, params[1].data_4, params[3].data_2, params[4].data_4))
				{
					r = -ENOMEM;
					goto fail;
				}
				break;
			}
			case KDB_CHDESC_CREATE_BYTE:
			{
				struct debug_param params[] = {
					{{.name = "chdesc"}},
					{{.name = "block"}},
					{{.name = "owner"}},
					{{.name = "offset"}},
					{{.name = "length"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					goto fail;
				assert(params[0].size == 4 && params[1].size == 4 &&
				       params[2].size == 4 && params[3].size == 2 &&
				       params[4].size == 2);
				if(!chdesc_create_byte(params[0].data_4, params[2].data_4, params[1].data_4, params[3].data_2, params[4].data_2))
				{
					r = -ENOMEM;
					goto fail;
				}
				break;
			}
			case KDB_CHDESC_CONVERT_NOOP:
				/* ... */
				break;
			case KDB_CHDESC_CONVERT_BIT:
				/* ... */
				break;
			case KDB_CHDESC_CONVERT_BYTE:
				/* ... */
				break;
			case KDB_CHDESC_REWRITE_BYTE:
				/* ... */
				break;
			case KDB_CHDESC_APPLY:
				/* ... */
				break;
			case KDB_CHDESC_ROLLBACK:
				/* ... */
				break;
			case KDB_CHDESC_SET_FLAGS:
				/* ... */
				break;
			case KDB_CHDESC_CLEAR_FLAGS:
				/* ... */
				break;
			case KDB_CHDESC_DESTROY:
			{
				struct debug_param params[] = {
					{{.name = "chdesc"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					goto fail;
				assert(params[0].size == 4);
				r = chdesc_destroy(params[0].data_4);
				if(r < 0)
					goto fail;
				break;
			}
			case KDB_CHDESC_ADD_BEFORE:
				/* ... */
				break;
			case KDB_CHDESC_ADD_AFTER:
				/* ... */
				break;
			case KDB_CHDESC_REM_BEFORE:
				/* ... */
				break;
			case KDB_CHDESC_REM_AFTER:
				/* ... */
				break;
			case KDB_CHDESC_WEAK_RETAIN:
				/* ... */
				break;
			case KDB_CHDESC_WEAK_FORGET:
				/* ... */
				break;
			case KDB_CHDESC_SET_OFFSET:
				/* ... */
				break;
			case KDB_CHDESC_SET_XOR:
				/* ... */
				break;
			case KDB_CHDESC_SET_LENGTH:
				/* ... */
				break;
			case KDB_CHDESC_SET_BLOCK:
				/* ... */
				break;
			case KDB_CHDESC_SET_OWNER:
				/* ... */
				break;
			case KDB_CHDESC_SET_FREE_PREV:
				/* ... */
				break;
			case KDB_CHDESC_SET_FREE_NEXT:
				/* ... */
				break;
			case KDB_CHDESC_SET_FREE_HEAD:
				/* ... */
				break;
			
			case KDB_CHDESC_SATISFY:
			case KDB_CHDESC_WEAK_COLLECT:
			case KDB_CHDESC_OVERLAP_ATTACH:
			case KDB_CHDESC_OVERLAP_MULTIATTACH:
				/* nothing */
				break;
			
		fail:
			printf("error %d applying opcode %d (%s)\n", -r, applied + 1, strerror(-r));
			put_opcode(&opcode);
			return r;
		}
		put_opcode(&opcode);
		applied++;
		progress++;
	}
	printf("\e[4D%d opcodes OK!\n", applied);
	return 0;
}

static int command_list(int argc, const char * argv[])
{
	int i, show_trace = 0;
	int min = 0, max = opcodes - 1;
	if(argc == 2)
	{
		/* show a single opcode */
		min = max = atoi(argv[1]) - 1;
		if(min < 0 || max >= opcodes)
		{
			printf("No such opcode.\n");
			return -1;
		}
		show_trace = 1;
	}
	else if(argc > 2)
	{
		/* show an opcode range */
		min = atoi(argv[1]) - 1;
		max = atoi(argv[2]) - 1;
		if(min < 0 || min > max)
		{
			printf("Invalid range.\n");
			return -1;
		}
		if(max >= opcodes)
			max = opcodes - 1;
	}
	for(i = min; i <= max; i++)
	{
		int m, o, p, r;
		struct debug_opcode opcode;
		r = get_opcode(i, &opcode);
		if(r < 0)
		{
			printf("Error %d reading opcode %d (%s)\n", -r, i + 1, strerror(-r));
			return r;
		}
		m = opcode.module_idx;
		o = opcode.opcode_idx;
		printf("#%d %s", i + 1, modules[m].opcodes[o]->name);
		for(p = 0; modules[m].opcodes[o]->params[p]->name; p++)
		{
			printf("%c %s = ", p ? ',' : ':', modules[m].opcodes[o]->params[p]->name);
			if(opcode.params[p].size == 4)
			{
				const char * format = "0x%08x";
				if(modules[m].opcodes[o]->params[p]->type == UINT32)
					format = "%u";
				else if(modules[m].opcodes[o]->params[p]->type == INT32)
					format = "%d";
				printf(format, opcode.params[p].data_4);
			}
			else if(opcode.params[p].size == 2)
			{
				if(modules[m].opcodes[o]->params[p]->type == UINT16)
					printf("%d", opcode.params[p].data_2);
				else if(modules[m].opcodes[o]->params[p]->type == INT16)
					printf("%d", (int) (int16_t) opcode.params[p].data_2);
				else
					printf("0x%04x", opcode.params[p].data_2);
			}
			else if(opcode.params[p].size == 1)
			{
				if(modules[m].opcodes[o]->params[p]->type == BOOL)
					printf(opcode.params[p].data_1 ? "true" : "false");
				else
					printf("%d", opcode.params[p].data_1);
			}
			else if(opcode.params[p].size == (uint8_t) -1)
			{
				printf("%s", opcode.params[p].data_v);
			}
		}
		printf("\n");
		printf("    from %s() at %s:%d\n", opcode.function, opcode.file, opcode.line);
		if(show_trace)
		{
			for(p = 0; opcode.stack[p]; p++)
				printf("  [%d]: 0x%08x", p, opcode.stack[p]);
			printf("\n");
		}
		put_opcode(&opcode);
	}
	return 0;
}

static int command_reset(int argc, const char * argv[])
{
	const char * array[] = {"jump", "0"};
	return command_jump(2, array);
}

static int command_run(int argc, const char * argv[])
{
	int r;
	char number[12];
	const char * array[] = {"jump", number};
	sprintf(number, "%u", opcodes);
	r = command_jump(2, array);
	if(r >= 0)
		printf("[Info: %d unique strings, %d unique stacks]\n", unique_strings, unique_stacks);
	return r;
}

static int command_status(int argc, const char * argv[])
{
	if(argc < 2)
	{
		printf("Debugging %s, read %d opcodes, applied %d\n", input_name, opcodes, applied);
		printf("[Info: %d chdescs, %d dependencies (%d raw)]\n", chdesc_count, (arrow_count + 1) / 2, arrow_count);
	}
	return 0;
}

static int command_step(int argc, const char * argv[])
{
	char number[12];
	const char * array[] = {"jump", number};
	int delta = (argc > 1) ? atoi(argv[1]) : 1;
	sprintf(number, "%u", applied + delta);
	return command_jump(2, array);
}

/* End commands */

/* Begin command line processing */

static int command_help(int argc, const char * argv[]);
static int command_quit(int argc, const char * argv[]);

struct {
	const char * command;
	const char * help;
	int (*execute)(int argc, const char * argv[]);
} commands[] = {
	//{"gui", "Start GUI control panel, optionally rendering to PostScript.", command_gui},
	{"jump", "Jump system state to a specified number of opcodes.", command_jump},
	{"list", "List opcodes in a specified range, or all opcodes by default.", command_list},
	//{"find", "Find max or min change descriptor count, optionally in an opcode range.", command_find},
	//{"option", "Get or set rendering options: freelist, grouping.", command_option},
	//{"ps", "Render system state to a PostScript file, or standard output by default.", command_ps},
	//{"render", "Render system state to a GraphViz dot file, or standard output by default.", command_render},
	{"reset", "Reset system state to 0 opcodes.", command_reset},
	{"run", "Apply all opcodes to system state.", command_run},
	{"status", "Displays system state status.", command_status},
	{"step", "Step system state by a specified number of opcodes, or 1 by default.", command_step},
	//{"view", "View system state graphically, optionally in a new window.", command_view},
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

static char * command_complete(const char * text, int state)
{
	static int index, length;
	if(!state)
	{
		int i;
		/* don't complete commands except at the beginning of the line */
		for(i = rl_point - 1; i >= 0; i--)
			if(rl_line_buffer[i] == ' ')
				return NULL;
		index = 0;
		length = strlen(text);
	}
	for(; index < COMMAND_COUNT; index++)
		if(!strncmp(commands[index].command, text, length))
			return strdup(commands[index++].command);
	return NULL;
}

static int command_line_execute(char * line)
{
	int i, argc = 0;
	const char * argv[64];
	do {
		while(*line == ' ')
			line++;
		if(!*line)
			break;
		argv[argc++] = line;
		while(*line && *line != ' ')
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

/* End command line processing */

int main(int argc, char * argv[])
{
	int r, percent = -1;
	struct stat file;
	off_t offset;
	
	if(argc < 2)
	{
		printf("Usage: %s <trace>\n", argv[0]);
		return 0;
	}
	
	r = stat(argv[1], &file);
	if(r < 0)
	{
		perror(argv[1]);
		return 1;
	}
	input_name = argv[1];
	input = fopen(input_name, "r");
	if(!input)
	{
		perror(input_name);
		return 1;
	}
	
	printf("Reading debug signature... ");
	fflush(stdout);
	r = read_debug_signature();
	if(r < 0)
	{
		printf("error %d (%s)\n", -r, strerror(-r));
		fclose(input);
		return 1;
	}
	else
		printf("OK!\n");
	
	printf("Scanning debugging output...     ");
	fflush(stdout);
	
	while((offset = ftello(input)) != file.st_size)
	{
		r = offset * 100 / file.st_size;
		if(r > percent)
		{
			percent = r;
			printf("\e[4D%2d%% ", percent);
			fflush(stdout);
		}
		
		r = scan_opcode();
		if(r < 0)
			break;
		add_opcode_offset(offset);
	}
	printf("\e[4D%d opcodes OK!\n", opcodes);
	if(r < 0)
		fprintf(stderr, "Error %d at file offset %lld+%lld (%s)\n", -r, offset, ftello(input) - offset, strerror(-r));
	
	if(opcodes)
	{
#if HASH_PRIME || RANDOM_TEST
		int opcode;
#endif
		printf("Average opcode length: %d bytes\n", (int) ((offset + opcodes / 2) / opcodes));
		
#if HASH_PRIME
		printf("Reading debugging output...     ");
		fflush(stdout);
		
		percent = -1;
		for(opcode = 0; opcode < opcodes; opcode++)
		{
			struct debug_opcode debug_opcode;
			r = opcode * 100 / opcodes;
			if(r > percent)
			{
				percent = r;
				printf("\e[4D%2d%% ", percent);
				fflush(stdout);
			}
			
			r = get_opcode(opcode, &debug_opcode);
			if(r < 0)
				break;
			put_opcode(&debug_opcode);
		}
		printf("\e[4D%d unique strings, %d unique stacks\n", unique_strings, unique_stacks);
#endif
		
#if RANDOM_TEST
		printf("Reading random opcodes...     ");
		fflush(stdout);
		
		percent = -1;
		for(opcode = 0; opcode < opcodes; opcode++)
		{
			struct debug_opcode debug_opcode;
			r = opcode * 100 / opcodes;
			if(r > percent)
			{
				percent = r;
				printf("\e[4D%2d%% ", percent);
				fflush(stdout);
			}
			
			r = get_opcode((rand() * RAND_MAX + rand()) % opcodes, &debug_opcode);
			if(r < 0)
				break;
			put_opcode(&debug_opcode);
		}
		printf("\e[4DOK!\n");
#endif
		
		rl_completion_entry_function = command_complete;
		do {
			int i;
			char * line = readline("debug> ");
			if(!line)
			{
				printf("\n");
				break;
			}
			for(i = 0; line[i] == ' '; i++);
			if(line[i])
				add_history(line);
			r = command_line_execute(line);
			free(line);
			if(r == -E2BIG)
				printf("Too many tokens on command line!\n");
			else if(r == -ENOENT)
				printf("No such command.\n");
		} while(r != -EINTR);
	}
	
	fclose(input);
	return 0;
}
