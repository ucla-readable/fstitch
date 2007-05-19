#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <readline/readline.h>
#include <readline/history.h>

#include <gtk/gtk.h>

#define WANT_DEBUG_STRUCTURES 1
#include "kfs/debug_opcode.h"

#define CONSTANTS_ONLY 1
#include "kfs/chdesc.h"

/* Set HASH_PRIME to do an extra pass over the input file to prime the string
 * and stack hash tables, and to report on the result. */
#define HASH_PRIME 0

/* Set RANDOM_TEST to do a sequence of random opcode reads after loading an
 * input file to test the speed of non-sequential access. */
#define RANDOM_TEST 0

/* Begin unique immutable strings {{{ */

#define HASH_TABLE_SIZE 65521

static uint32_t strsum(const char * string)
{
	uint32_t sum = 0x5AFEDA7A;
	if(!string)
		return 0;
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
	if(!string)
	{
		for(index = 0; index < HASH_TABLE_SIZE; index++)
			while(hash_table[index])
			{
				struct hash_entry * old = hash_table[index];
				hash_table[index] = old->next;
				free((void *) old->string);
				free(old);
			}
		return NULL;
	}
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

/* End unique immutable strings }}} */

/* Begin unique immutable stacks {{{ */

static uint32_t stksum(const uint32_t * stack)
{
	uint32_t sum = 0x5AFEDA7A;
	if(!stack)
		return 0;
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
	if(!stack)
	{
		for(index = 0; index < HASH_TABLE_SIZE; index++)
			while(hash_table[index])
			{
				struct hash_entry * old = hash_table[index];
				hash_table[index] = old->next;
				free((void *) old->stack);
				free(old);
			}
		return NULL;
	}
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

/* End unique immutable stacks }}} */

/* Begin opcode reading {{{ */

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

static FILE * input_file = NULL;
static const char * input_name = NULL;

static uint8_t input_buffer[32768];
static int input_buffer_size = 0;
static int input_buffer_pos = 0;
static int input_eof = 0;

static int input_init(const char * name)
{
	input_file = fopen(name, "r");
	if(!input_file)
		return -errno;
	input_name = name;
	return 0;
}

static uint8_t input_uint8(void) __attribute__((always_inline));
static uint8_t input_uint8(void)
{
	if(input_buffer_pos >= input_buffer_size)
	{
		input_buffer_size = fread(input_buffer, 1, sizeof(input_buffer), input_file);
		if(input_buffer_size <= 0)
		{
			input_eof = 1;
			return -1;
		}
		input_buffer_pos = 0;
	}
	return input_buffer[input_buffer_pos++];
}

static off_t input_offset(void)
{
	if(input_eof)
		return ftello(input_file);
	return ftello(input_file) - input_buffer_size + input_buffer_pos;
}

static void input_seek(off_t offset)
{
	input_buffer_size = 0;
	input_buffer_pos = 0;
	input_eof = 0;
	fseeko(input_file, offset, SEEK_SET);
}

static void input_finish(void)
{
	fclose(input_file);
	input_file = NULL;
	input_name = NULL;
}

static int read_lit_8(uint8_t * data)
{
	*data = input_uint8();
	if(input_eof)
		return -1;
	return 0;
}

static int read_lit_16(uint16_t * data)
{
	*data = input_uint8();
	if(input_eof)
		return -1;
	*data <<= 8;
	*data |= input_uint8();
	if(input_eof)
		return -1;
	return 0;
}

/* This function is called an order of magnitude more frequently than any other
 * function. It is important that it be very fast. So, unroll the obvious loop. */
static int read_lit_32(uint32_t * data)
{
	*data = input_uint8();
	if(input_eof)
		return -1;
	*data <<= 8;
	*data |= input_uint8();
	if(input_eof)
		return -1;
	*data <<= 8;
	*data |= input_uint8();
	if(input_eof)
		return -1;
	*data <<= 8;
	*data |= input_uint8();
	if(input_eof)
		return -1;
	return 0;
}

static int read_lit_str(const char ** data, int allocate)
{
	int i;
	static char buffer[128];
	for(i = 0; i < sizeof(buffer); i++)
	{
		buffer[i] = input_uint8();
		if(input_eof)
			return -1;
		if(!buffer[i])
			break;
		/* convert double quotes to single quotes */
		if(buffer[i] == '"')
			buffer[i] = '\'';
	}
	*data = allocate ? strdup_unique(buffer) : buffer;
	return 0;
}

static uint32_t debug_rev, debug_opcode_rev;

static int read_debug_signature(void)
{
	int m, o, r;
	uint16_t zero;
	
	r = read_lit_32(&debug_rev);
	if(r < 0)
		return r;
	r = read_lit_32(&debug_opcode_rev);
	if(r < 0)
		return r;
	if((debug_rev != 3408 && debug_rev != 3439 && debug_rev != 3456) || debug_opcode_rev != 3414)
		return -EPROTO;
	if(debug_rev != 3456)
		printf("[detected old file; cache command may not work] ");
	
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
		input_seek(get_opcode_offset(index));
	last_index = index;
	return read_opcode(debug_opcode);
}

static void put_opcode(struct debug_opcode * debug_opcode)
{
	free(debug_opcode->params);
}

/* End opcode reading }}} */

/* Begin state management {{{ */

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

struct weak {
	uint32_t location;
	struct weak * next;
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
	uint16_t flags, local_flags;
	struct weak * weak_refs;
	struct arrow * befores;
	struct arrow * afters;
	struct label * labels;
	uint32_t free_prev, free_next;
	/* hash table */
	struct chdesc * next;
	/* groups */
	struct chdesc * group_next[2];
};

static const char * type_names[] = {[BIT] = "BIT", [BYTE] = "BYTE", [NOOP] = "NOOP"};

static struct bd * bds = NULL;
static struct block * blocks[HASH_TABLE_SIZE];
static struct chdesc * chdescs[HASH_TABLE_SIZE];
static uint32_t chdesc_free_head = 0;
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
	int i;
	while(bds)
	{
		struct bd * old = bds;
		bds = old->next;
		free(old);
	}
	for(i = 0; i < HASH_TABLE_SIZE; i++)
		while(blocks[i])
		{
			struct block * old = blocks[i];
			blocks[i] = old->next;
			free(old);
		}
	for(i = 0; i < HASH_TABLE_SIZE; i++)
		while(chdescs[i])
		{
			struct chdesc * old = chdescs[i];
			chdescs[i] = old->next;
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
	int index = address % HASH_TABLE_SIZE;
	for(scan = blocks[index]; scan; scan = scan->next)
		if(scan->address == address)
			break;
	return scan;
}

static struct chdesc * lookup_chdesc(uint32_t address)
{
	struct chdesc * scan;
	int index = address % HASH_TABLE_SIZE;
	for(scan = chdescs[index]; scan; scan = scan->next)
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
	int index = address % HASH_TABLE_SIZE;
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
	block->next = blocks[index];
	blocks[index] = block;
	return 0;
}

static struct chdesc * _chdesc_create(uint32_t address, uint32_t owner)
{
	int index = address % HASH_TABLE_SIZE;
	struct chdesc * chdesc = malloc(sizeof(*chdesc));
	if(!chdesc)
		return NULL;
	chdesc->address = address;
	chdesc->opcode = applied + 1;
	chdesc->owner = owner;
	chdesc->flags = 0;
	chdesc->local_flags = 0;
	chdesc->weak_refs = NULL;
	chdesc->befores = NULL;
	chdesc->afters = NULL;
	chdesc->labels = NULL;
	chdesc->free_prev = 0;
	chdesc->free_next = 0;
	chdesc->next = chdescs[index];
	chdescs[index] = chdesc;
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

static int chdesc_add_weak(struct chdesc * chdesc, uint32_t location)
{
	struct weak * weak = malloc(sizeof(*weak));
	if(!weak)
		return -ENOMEM;
	weak->location = location;
	weak->next = chdesc->weak_refs;
	chdesc->weak_refs = weak;
	return 0;
}

static int chdesc_rem_weak(struct chdesc * chdesc, uint32_t location)
{
	struct weak ** point;
	for(point = &chdesc->weak_refs; *point; point = &(*point)->next)
		if((*point)->location == location)
		{
			struct weak * old = *point;
			*point = old->next;
			free(old);
			return 0;
		}
	return -ENOENT;
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

static int chdesc_add_after(struct chdesc * before, uint32_t after)
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

static int chdesc_rem_after(struct chdesc * before, uint32_t after)
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
	int index = address % HASH_TABLE_SIZE;
	struct chdesc ** point;
	for(point = &chdescs[index]; *point; point = &(*point)->next)
		if((*point)->address == address)
		{
			struct chdesc * old = *point;
			*point = old->next;
			free_arrows(&old->befores);
			free_arrows(&old->afters);
			free_labels(&old->labels);
			free(old);
			chdesc_count--;
			return 0;
		}
	return -ENOENT;
}

/* End state management }}} */

/* Begin rendering and grouping {{{ */

static int render_free = 0;
static int render_block = 1;
static int render_owner = 1;
static enum grouping_type {
	OFF,
	BLOCK,
	OWNER,
	BLOCK_OWNER,
	OWNER_BLOCK
} current_grouping = OFF;
static struct {
	const char * name;
	enum grouping_type type;
} groupings[] = {
	{"off", OFF},
	{"block", BLOCK},
	{"owner", OWNER},
	{"block-owner", BLOCK_OWNER},
	{"owner-block", OWNER_BLOCK}
};
#define GROUPINGS (sizeof(groupings) / sizeof(groupings[0]))
static const char * grouping_names[] = {
	[OFF] = "off",
	[BLOCK] = "block[red]",
	[OWNER] = "owner[red]",
	[BLOCK_OWNER] = "block[gold]-owner[red]",
	[OWNER_BLOCK] = "owner[gold]-block[red]"
};

struct group_hash;
struct group {
	uint32_t key;
	struct chdesc * chdescs;
	struct group_hash * sub;
	struct group * next;
};

struct group_hash {
	struct group * hash_table[HASH_TABLE_SIZE];
};

static struct group_hash * groups = NULL;

static struct group * chdesc_group_key(struct group_hash * hash, uint32_t key, int level, struct chdesc * chdesc)
{
	int index = key % HASH_TABLE_SIZE;
	struct group * group;
	for(group = hash->hash_table[index]; group; group = group->next)
		if(group->key == key)
			break;
	if(!group)
	{
		group = malloc(sizeof(*group));
		group->key = key;
		group->chdescs = NULL;
		group->sub = NULL;
		group->next = hash->hash_table[index];
		hash->hash_table[index] = group;
	}
	chdesc->group_next[level] = group->chdescs;
	group->chdescs = chdesc;
	return group;
}

static int chdesc_group(struct chdesc * chdesc)
{
	uint32_t key = 0;
	struct group * group;
	if(current_grouping == OFF)
		return 0;
	if(!groups)
	{
		groups = calloc(1, sizeof(*groups));
		if(!groups)
			return -ENOMEM;
	}
	if(current_grouping == BLOCK || current_grouping == BLOCK_OWNER)
		key = chdesc->block;
	else if(current_grouping == OWNER || current_grouping == OWNER_BLOCK)
		key = chdesc->owner;
	group = chdesc_group_key(groups, key, 0, chdesc);
	if(!group)
		return -ENOMEM;
	if(current_grouping == BLOCK_OWNER || current_grouping == OWNER_BLOCK)
	{
		key = 0;
		if(!group->sub)
		{
			group->sub = calloc(1, sizeof(*group->sub));
			if(!group->sub)
				return -ENOMEM;
		}
		if(current_grouping == BLOCK_OWNER)
			key = chdesc->owner;
		else if(current_grouping == OWNER_BLOCK)
			key = chdesc->block;
		group = chdesc_group_key(group->sub, key, 1, chdesc);
		if(!group)
			return -ENOMEM;
	}
	return 0;
}

static void kill_group(struct group_hash * hash)
{
	int i;
	for(i = 0; i < HASH_TABLE_SIZE; i++)
		while(hash->hash_table[i])
		{
			struct group * old = hash->hash_table[i];
			hash->hash_table[i] = old->next;
			if(old->sub)
				kill_group(old->sub);
			free(old);
		}
}

static void reset_groups(void)
{
	if(groups)
	{
		kill_group(groups);
		groups = NULL;
	}
}

static int render_group(FILE * output, struct group * group, int level)
{
	const char * color = "red";
	if(group->key)
	{
		fprintf(output, "subgraph cluster%dL0x%08x {\n", level, group->key);
		if((level == 0 && (current_grouping == BLOCK || current_grouping == BLOCK_OWNER)) || (level == 1 && current_grouping == OWNER_BLOCK))
		{
			/* render a block group */
			struct block * block = lookup_block(group->key);
			if(block)
				fprintf(output, "label=\"#%d (0x%08x)\";\n", block->number, block->address);
			else
				fprintf(output, "label=\"0x%08x\";\n", group->key);
		}
		else if((level == 0 && (current_grouping == OWNER || current_grouping == OWNER_BLOCK)) || (level == 1 && current_grouping == BLOCK_OWNER))
		{
			/* render an owner group */
			struct bd * bd = lookup_bd(group->key);
			if(bd)
				fprintf(output, "label=\"%s\";\n", bd->name);
			else
				fprintf(output, "label=\"0x%08x\";\n", group->key);
		}
		else
			return -1;
		if(level == 0 && (current_grouping == BLOCK_OWNER || current_grouping == OWNER_BLOCK))
			color = "gold";
		fprintf(output, "color=%s;\nlabeljust=r;\n", color);
	}
	if(level == 1 || current_grouping == BLOCK || current_grouping == OWNER)
	{
		/* actually list the chdescs */
		struct chdesc * chdesc;
		for(chdesc = group->chdescs; chdesc; chdesc = chdesc->group_next[level])
			fprintf(output, "\"ch0x%08x-hc%p\"\n", chdesc->address, (void *) chdesc);
	}
	return !!group->key;
}

static void render_groups(FILE * output)
{
	int i;
	if(current_grouping == OFF)
		return;
	for(i = 0; i < HASH_TABLE_SIZE; i++)
	{
		struct group * group = groups->hash_table[i];
		for(; group; group = group->next)
		{
			int r = render_group(output, group, 0);
			assert(r >= 0);
			if(current_grouping == BLOCK_OWNER || current_grouping == OWNER_BLOCK)
			{
				int j;
				assert(group->sub);
				for(j = 0; j < HASH_TABLE_SIZE; j++)
				{
					struct group * sub = group->sub->hash_table[j];
					for(; sub; sub = sub->next)
					{
						int s = render_group(output, sub, 1);
						assert(s >= 0);
						if(s)
							fprintf(output, "}\n");
					}
				}
			}
			if(r)
				fprintf(output, "}\n");
		}
	}
}

static void render_block_owner(FILE * output, struct chdesc * chdesc)
{
	if(chdesc->block && render_block)
	{
		struct block * block = lookup_block(chdesc->block);
		if(block)
			fprintf(output, "\\n#%d (0x%08x)", block->number, block->address);
		else
			fprintf(output, "\\non 0x%08x", chdesc->block);
	}
	if(chdesc->owner && render_owner)
	{
		struct bd * bd = lookup_bd(chdesc->owner);
		if(bd)
			fprintf(output, "\\n%s", bd->name);
		else
			fprintf(output, "\\nat 0x%08x", chdesc->owner);
	}
}

static void render_chdesc(FILE * output, struct chdesc * chdesc, int render_free)
{
	struct label * label;
	struct arrow * arrow;
	struct weak * weak;
	
	fprintf(output, "\"ch0x%08x-hc%p\" [label=\"0x%08x", chdesc->address, (void *) chdesc, chdesc->address);
	for(label = chdesc->labels; label; label = label->next)
		fprintf(output, "\\n\\\"%s\\\"", label->label);
	switch(chdesc->type)
	{
		case NOOP:
			render_block_owner(output, chdesc);
			fprintf(output, "\",style=\"");
			break;
		case BIT:
			fprintf(output, "\\n[%d:0x%08x]", chdesc->bit.offset, chdesc->bit.xor);
			render_block_owner(output, chdesc);
			fprintf(output, "\",fillcolor=springgreen1,style=\"filled");
			break;
		case BYTE:
			fprintf(output, "\\n[%d:%d]", chdesc->byte.offset, chdesc->byte.length);
			render_block_owner(output, chdesc);
			fprintf(output, "\",fillcolor=slateblue1,style=\"filled");
			break;
	}
	if(chdesc->flags & CHDESC_ROLLBACK)
		fprintf(output, ",dashed,bold");
	if(chdesc->flags & CHDESC_MARKED)
		fprintf(output, ",bold\",color=red");
	else
		fprintf(output, "\"");
	if(chdesc->flags & CHDESC_FREEING)
		fprintf(output, ",fontcolor=red");
	else if(chdesc->flags & CHDESC_WRITTEN)
		fprintf(output, ",fontcolor=blue");
	fprintf(output, "]\n");
	
	for(arrow = chdesc->befores; arrow; arrow = arrow->next)
	{
		struct chdesc * before = lookup_chdesc(arrow->chdesc);
		if(before)
			fprintf(output, "\"ch0x%08x-hc%p\" -> \"ch0x%08x-hc%p\" [color=black]\n", chdesc->address, (void *) chdesc, before->address, (void *) before);
	}
	for(arrow = chdesc->afters; arrow; arrow = arrow->next)
	{
		struct chdesc * after = lookup_chdesc(arrow->chdesc);
		if(after)
			fprintf(output, "\"ch0x%08x-hc%p\" -> \"ch0x%08x-hc%p\" [color=gray]\n", after->address, (void *) after, chdesc->address, (void *) chdesc);
	}
	for(weak = chdesc->weak_refs; weak; weak = weak->next)
	{
		fprintf(output, "\"0x%08x\" [shape=box,fillcolor=yellow,style=filled]\n", weak->location);
		fprintf(output, "\"0x%08x\" -> \"ch0x%08x-hc%p\" [color=green]\n", weak->location, chdesc->address, (void *) chdesc);
	}
	if(chdesc->free_prev)
	{
		struct chdesc * prev = lookup_chdesc(chdesc->free_prev);
		if(prev)
			fprintf(output, "\"ch0x%08x-hc%p\" -> \"ch0x%08x-hc%p\" [color=orange]\n", prev->address, (void *) prev, chdesc->address, (void *) chdesc);
	}
	if(chdesc->free_next && render_free)
	{
		struct chdesc * next = lookup_chdesc(chdesc->free_next);
		if(next)
			fprintf(output, "\"ch0x%08x-hc%p\" -> \"ch0x%08x-hc%p\" [color=red]\n", chdesc->address, (void *) chdesc, next->address, (void *) next);
	}
}

static void render(FILE * output, const char * title, int landscape)
{
	int i, free = 0;
	
	/* header */
	fprintf(output, "digraph \"debug: %d/%d opcode%s, %s\"\n", applied, opcodes, (opcodes == 1) ? "" : "s", input_name);
	fprintf(output, "{\nnodesep=0.25;\nranksep=0.25;\n");
	if(landscape)
		fprintf(output, "rankdir=LR;\norientation=L;\nsize=\"10,7.5\";\n");
	else
		fprintf(output, "rankdir=LR;\norientation=P;\nsize=\"16,16\";\n");
	fprintf(output, "subgraph clusterAll {\nlabel=\"%s\";\ncolor=white;\n", title);
	fprintf(output, "node [shape=ellipse,color=black];\n");
	
	for(i = 0; i < HASH_TABLE_SIZE; i++)
	{
		struct chdesc * chdesc;
		for(chdesc = chdescs[i]; chdesc; chdesc = chdesc->next)
		{
			int is_free = chdesc->address == chdesc_free_head || chdesc->free_prev;
			if(is_free)
				free++;
			if(render_free)
			{
				if(!(chdesc->flags & CHDESC_WRITTEN))
				{
					int r = chdesc_group(chdesc);
					assert(r >= 0);
				}
				render_chdesc(output, chdesc, 1);
			}
			else if(chdesc->address == chdesc_free_head || !chdesc->free_prev)
			{
				if(!(chdesc->flags & CHDESC_WRITTEN))
				{
					int r = chdesc_group(chdesc);
					assert(r >= 0);
				}
				render_chdesc(output, chdesc, 0);
			}
		}
	}
	
	if(chdesc_free_head)
	{
		fprintf(output, "subgraph cluster_free {\ncolor=red;\nstyle=dashed;\n");
		if(render_free)
		{
			struct chdesc * chdesc = lookup_chdesc(chdesc_free_head);
			fprintf(output, "label=\"Free List\";\n");
			while(chdesc)
			{
				fprintf(output, "\"ch0x%08x-hc%p\"\n", chdesc->address, (void *) chdesc);
				chdesc = lookup_chdesc(chdesc->free_next);
			}
			if(free > 3)
			{
				double ratio = sqrt(free / 1.6) / free;
				int cluster = 0;
				free = 0;
				fprintf(output, "subgraph cluster_align {\nstyle=invis;\n");
				chdesc = lookup_chdesc(chdesc_free_head);
				while(chdesc)
				{
					free++;
					if(cluster < ratio * free)
					{
						cluster++;
						fprintf(output, "\"ch0x%08x-hc%p\"\n", chdesc->address, (void *) chdesc);
					}
					chdesc = lookup_chdesc(chdesc->free_next);
				}
				fprintf(output, "}\n");
			}
		}
		else
		{
			fprintf(output, "label=\"Free Head (+%d)\";\n", free - 1);
			fprintf(output, "\"ch0x%08x-hc%p\"\n", chdesc_free_head, (void *) lookup_chdesc(chdesc_free_head));
		}
		fprintf(output, "}\n");
	}
	
	/* grouping */
	render_groups(output);
	reset_groups();
	
	/* footer */
	fprintf(output, "}\n}\n");
}

/* End rendering and grouping }}} */

/* Begin opcode logic {{{ */

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

static int param_chdesc_int_apply(struct debug_opcode * opcode, const char * name1, const char * name2, int (*apply)(struct chdesc *, uint32_t))
{
	int r;
	struct chdesc * chdesc;
	struct debug_param params[3];
	params[0].name = name1;
	params[1].name = name2;
	params[2].name = NULL;
	r = param_lookup(opcode, params);
	if(r < 0)
		return r;
	assert(params[0].size == 4 && params[1].size == 4);
	chdesc = lookup_chdesc(params[0].data_4);
	if(!chdesc)
		return -EFAULT;
	return apply(chdesc, params[1].data_4);
}

#ifndef ptrdiff_t
#define ptrdiff_t uintptr_t
#endif
#define field_offset(struct, field) ((ptrdiff_t) &((struct *) NULL)->field)

static int param_chdesc_set_field(struct debug_opcode * opcode, const char * name1, const char * name2, ptrdiff_t field)
{
	int r;
	struct chdesc * chdesc;
	struct debug_param params[3];
	params[0].name = name1;
	params[1].name = name2;
	params[2].name = NULL;
	r = param_lookup(opcode, params);
	if(r < 0)
		return r;
	assert(params[0].size == 4 && params[1].size == 4);
	chdesc = lookup_chdesc(params[0].data_4);
	if(!chdesc)
		return -EFAULT;
	/* looks ugly but it's the only way */
	*(uint32_t *) (((uintptr_t) chdesc) + field) = params[1].data_4;
	return 0;
}

/* This function contains almost all of the opcode-specific code. Everything
 * else is driven by tables defined in debug_opcode.h or in this function. */
static int apply_opcode(struct debug_opcode * opcode, int * effect, int * skippable)
{
	int m, o, r = 0;
	m = opcode->module_idx;
	o = opcode->opcode_idx;
	if(effect)
		*effect = 1;
	if(skippable)
		*skippable = 0;
	switch(modules[m].opcodes[o]->opcode)
	{
		case KDB_INFO_MARK:
			/* nothing */
			if(effect)
				*effect = 0;
			break;
		case KDB_INFO_BD_NAME:
		{
			struct debug_param params[] = {
				{{.name = "bd"}},
				{{.name = "name"}},
				{{.name = NULL}}
			};
			if(skippable)
				*skippable = 1;
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == (uint8_t) -1);
			r = add_bd_name(params[0].data_4, params[1].data_v);
			break;
		}
		case KDB_INFO_BDESC_NUMBER:
		{
			struct debug_param params[] = {
				{{.name = "block"}},
				{{.name = "number"}},
				{{.name = NULL}}
			};
			if(skippable)
				*skippable = 1;
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			r = add_block_number(params[0].data_4, params[1].data_4);
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
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == (uint8_t) -1);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			r = chdesc_add_label(chdesc, params[1].data_v);
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
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			if(!chdesc_create_noop(params[0].data_4, params[1].data_4))
				r = -ENOMEM;
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
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4 &&
					params[2].size == 4 && params[3].size == 2 &&
					params[4].size == 4);
			if(!chdesc_create_bit(params[0].data_4, params[2].data_4, params[1].data_4, params[3].data_2, params[4].data_4))
				r = -ENOMEM;
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
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4 &&
					params[2].size == 4 && params[3].size == 2 &&
					params[4].size == 2);
			if(!chdesc_create_byte(params[0].data_4, params[2].data_4, params[1].data_4, params[3].data_2, params[4].data_2))
				r = -ENOMEM;
			break;
		}
		case KDB_CHDESC_CONVERT_NOOP:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			chdesc->type = NOOP;
			break;
		}
		case KDB_CHDESC_CONVERT_BIT:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = "offset"}},
				{{.name = "xor"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 2 &&
					params[2].size == 4);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			chdesc->type = BIT;
			chdesc->bit.offset = params[1].data_2;
			chdesc->bit.xor = params[2].data_4;
			break;
		}
		case KDB_CHDESC_CONVERT_BYTE:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = "offset"}},
				{{.name = "length"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 2 &&
					params[2].size == 2);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			chdesc->type = BYTE;
			chdesc->byte.offset = params[1].data_2;
			chdesc->byte.length = params[2].data_2;
			break;
		}
		case KDB_CHDESC_REWRITE_BYTE:
			/* nothing */
			break;
		case KDB_CHDESC_APPLY:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			chdesc->flags &= ~CHDESC_ROLLBACK;
			break;
		}
		case KDB_CHDESC_ROLLBACK:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			chdesc->flags |= CHDESC_ROLLBACK;
			break;
		}
		case KDB_CHDESC_SET_FLAGS:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = "flags"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			chdesc->flags |= params[1].data_4;
			break;
		}
		case KDB_CHDESC_CLEAR_FLAGS:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = "flags"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			chdesc->flags &= ~params[1].data_4;
			break;
		}
		case KDB_CHDESC_DESTROY:
		{
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			r = chdesc_destroy(params[0].data_4);
			break;
		}
		case KDB_CHDESC_ADD_BEFORE:
			r = param_chdesc_int_apply(opcode, "source", "target", chdesc_add_before);
			break;
		case KDB_CHDESC_ADD_AFTER:
			r = param_chdesc_int_apply(opcode, "source", "target", chdesc_add_after);
			break;
		case KDB_CHDESC_REM_BEFORE:
			r = param_chdesc_int_apply(opcode, "source", "target", chdesc_rem_before);
			break;
		case KDB_CHDESC_REM_AFTER:
			r = param_chdesc_int_apply(opcode, "source", "target", chdesc_rem_after);
			break;
		case KDB_CHDESC_WEAK_RETAIN:
			r = param_chdesc_int_apply(opcode, "chdesc", "location", chdesc_add_weak);
			break;
		case KDB_CHDESC_WEAK_FORGET:
			r = param_chdesc_int_apply(opcode, "chdesc", "location", chdesc_rem_weak);
			break;
		case KDB_CHDESC_SET_OFFSET:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = "offset"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 2);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			if(chdesc->type == BIT)
				chdesc->bit.offset = params[1].data_2;
			else if(chdesc->type == BYTE)
				chdesc->byte.offset = params[1].data_2;
			else
				r = -ENOMSG;
			break;
		}
		case KDB_CHDESC_SET_XOR:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = "xor"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			if(chdesc->type != BIT)
			{
				r = -ENOMSG;
				break;
			}
			chdesc->bit.xor = params[1].data_4;
			break;
		}
		case KDB_CHDESC_SET_LENGTH:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = "length"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 2);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			if(chdesc->type != BYTE)
			{
				r = -ENOMSG;
				break;
			}
			chdesc->byte.length = params[1].data_2;
			break;
		}
		case KDB_CHDESC_SET_BLOCK:
		{
			struct chdesc * chdesc;
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = "block"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			chdesc = lookup_chdesc(params[0].data_4);
			if(!chdesc)
			{
				r = -EFAULT;
				break;
			}
			if(chdesc->type != BIT && chdesc->type != BYTE && params[1].data_4)
			{
				r = -ENOMSG;
				break;
			}
			chdesc->block = params[1].data_4;
			break;
		}
		case KDB_CHDESC_SET_OWNER:
			r = param_chdesc_set_field(opcode, "chdesc", "owner", field_offset(struct chdesc, owner));
			break;
		case KDB_CHDESC_SET_FREE_PREV:
			r = param_chdesc_set_field(opcode, "chdesc", "free_prev", field_offset(struct chdesc, free_prev));
			break;
		case KDB_CHDESC_SET_FREE_NEXT:
			r = param_chdesc_set_field(opcode, "chdesc", "free_next", field_offset(struct chdesc, free_next));
			break;
		case KDB_CHDESC_SET_FREE_HEAD:
		{
			struct debug_param params[] = {
				{{.name = "chdesc"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			chdesc_free_head = params[0].data_4;
			break;
		}
		
		case KDB_CHDESC_SATISFY:
		case KDB_CHDESC_WEAK_COLLECT:
		case KDB_CHDESC_OVERLAP_ATTACH:
		case KDB_CHDESC_OVERLAP_MULTIATTACH:
			/* nothing */
			if(effect)
				*effect = 0;
			break;
		
		case KDB_CACHE_NOTIFY:
		case KDB_CACHE_FINDBLOCK:
		case KDB_CACHE_LOOKBLOCK:
		case KDB_CACHE_WRITEBLOCK:
			/* ... */
			if(effect)
				*effect = 0;
			break;
	}
	return r;
}

static void print_opcode(int number, struct debug_opcode * opcode, int show_trace)
{
	int m, o, p;
	m = opcode->module_idx;
	o = opcode->opcode_idx;
	printf("#%d %s", number, modules[m].opcodes[o]->name);
	for(p = 0; modules[m].opcodes[o]->params[p]->name; p++)
	{
		printf("%c %s = ", p ? ',' : ':', modules[m].opcodes[o]->params[p]->name);
		if(opcode->params[p].size == 4)
		{
			const char * format = "0x%08x";
			if(modules[m].opcodes[o]->params[p]->type == UINT32)
				format = "%u";
			else if(modules[m].opcodes[o]->params[p]->type == INT32)
				format = "%d";
			printf(format, opcode->params[p].data_4);
		}
		else if(opcode->params[p].size == 2)
		{
			if(modules[m].opcodes[o]->params[p]->type == UINT16)
				printf("%d", opcode->params[p].data_2);
			else if(modules[m].opcodes[o]->params[p]->type == INT16)
				printf("%d", (int) (int16_t) opcode->params[p].data_2);
			else
				printf("0x%04x", opcode->params[p].data_2);
		}
		else if(opcode->params[p].size == 1)
		{
			if(modules[m].opcodes[o]->params[p]->type == BOOL)
				printf(opcode->params[p].data_1 ? "true" : "false");
			else
				printf("%d", opcode->params[p].data_1);
		}
		else if(opcode->params[p].size == (uint8_t) -1)
			printf("%s", opcode->params[p].data_v);
	}
	if(opcode->function[0] || opcode->file[0])
		printf("\n    from %s() at %s:%d\n", opcode->function, opcode->file, opcode->line);
	else
		printf(" (line %d)\n", opcode->line);
	if(show_trace)
	{
		for(p = 0; opcode->stack[p]; p++)
			printf("  [%d]: 0x%08x", p, opcode->stack[p]);
		printf("\n");
	}
}

static int snprint_opcode(char * string, size_t length, struct debug_opcode * opcode)
{
	int m, o, p, r;
	m = opcode->module_idx;
	o = opcode->opcode_idx;
	r = snprintf(string, length, "%s", modules[m].opcodes[o]->name);
	if(r >= length)
		return -1;
	length -= r;
	string += r;
	for(p = 0; modules[m].opcodes[o]->params[p]->name; p++)
	{
		r = snprintf(string, length, "%c %s = ", p ? ',' : ':', modules[m].opcodes[o]->params[p]->name);
		if(r >= length)
			return -1;
		length -= r;
		string += r;
		if(opcode->params[p].size == 4)
		{
			const char * format = "0x%08x";
			if(modules[m].opcodes[o]->params[p]->type == UINT32)
				format = "%u";
			else if(modules[m].opcodes[o]->params[p]->type == INT32)
				format = "%d";
			r = snprintf(string, length, format, opcode->params[p].data_4);
		}
		else if(opcode->params[p].size == 2)
		{
			if(modules[m].opcodes[o]->params[p]->type == UINT16)
				r = snprintf(string, length, "%d", opcode->params[p].data_2);
			else if(modules[m].opcodes[o]->params[p]->type == INT16)
				r = snprintf(string, length, "%d", (int) (int16_t) opcode->params[p].data_2);
			else
				r = snprintf(string, length, "0x%04x", opcode->params[p].data_2);
		}
		else if(opcode->params[p].size == 1)
		{
			if(modules[m].opcodes[o]->params[p]->type == BOOL)
				r = snprintf(string, length, opcode->params[p].data_1 ? "true" : "false");
			else
				r = snprintf(string, length, "%d", opcode->params[p].data_1);
		}
		else if(opcode->params[p].size == (uint8_t) -1)
			r = snprintf(string, length, "%s", opcode->params[p].data_v);
		else
			continue;
		if(r >= length)
			return -1;
		length -= r;
		string += r;
	}
	return 0;
}

/* End opcode logic }}} */

/* Begin cache analysis {{{ */

#define CACHE_CHDESC_READY    0x01

#define CACHE_BLOCK_DIRTY     0x01
#define CACHE_BLOCK_INFLIGHT  0x02
#define CACHE_BLOCK_READY     0x04
#define CACHE_BLOCK_HALFREADY 0x08
#define CACHE_BLOCK_NOTREADY  0x10

static struct cache_block {
	uint32_t address;
	uint32_t local_flags;
	struct block * block;
	struct chdesc * chdescs;
	int chdesc_count;
	struct chdesc * ready;
	int ready_count;
	int dep_count;
	int dblock_count;
	uint32_t dblock_last;
	struct cache_block * next;
} * cache_blocks[HASH_TABLE_SIZE];

struct cache_situation {
	int dirty, inflight, dirty_inflight;
	int full_ready, half_ready, not_ready;
};

struct cache_choice {
	int choices;
	int look_ready, look_half, look_not;
	int write_ready, write_half;
	int write_rdeps, write_rdblocks;
	int write_hdeps, write_hdblocks;
};

static struct cache_block * cache_block_lookup(uint32_t address)
{
	struct cache_block * scan;
	int index = address % HASH_TABLE_SIZE;
	for(scan = cache_blocks[index]; scan; scan = scan->next)
		if(scan->address == address)
			return scan;
	scan = malloc(sizeof(*scan));
	if(!scan)
		return NULL;
	scan->address = address;
	scan->local_flags = 0;
	scan->block = lookup_block(address);
	scan->chdescs = NULL;
	scan->chdesc_count = 0;
	scan->ready = NULL;
	scan->ready_count = 0;
	scan->dep_count = 0;
	scan->dblock_count = 0;
	scan->dblock_last = 0;
	scan->next = cache_blocks[index];
	cache_blocks[index] = scan;
	return scan;
}

static void cache_block_clean(void)
{
	int i;
	for(i = 0; i < HASH_TABLE_SIZE; i++)
		while(cache_blocks[i])
		{
			struct cache_block * old = cache_blocks[i];
			cache_blocks[i] = old->next;
			free(old);
		}
}

static int chdesc_is_ready(struct chdesc * chdesc, uint32_t block)
{
	struct arrow * scan;
	for(scan = chdesc->befores; scan; scan = scan->next)
	{
		struct chdesc * before = lookup_chdesc(scan->chdesc);
		assert(before);
		if(before->type == NOOP)
		{
			/* recursive */
			if(!chdesc_is_ready(before, block))
				return 0;
		}
		else if(before->block != block)
			return 0;
		else if(!(before->local_flags & CACHE_CHDESC_READY))
			return 0;
	}
	return 1;
}

static void dblock_update(struct chdesc * depender, struct chdesc * chdesc)
{
	if(depender == chdesc || chdesc->type == NOOP)
	{
		struct arrow * scan;
		for(scan = chdesc->befores; scan; scan = scan->next)
		{
			struct chdesc * before = lookup_chdesc(scan->chdesc);
			assert(before);
			dblock_update(depender, before);
		}
	}
	else if(chdesc->block != depender->block)
	{
		struct cache_block * block = cache_block_lookup(chdesc->block);
		block->dep_count++;
		if(block->dblock_last != depender->block)
		{
			block->dblock_count++;
			block->dblock_last = depender->block;
		}
	}
}

/* Look at all change descriptors, and determine which blocks on the given cache
 * can be completely written, partially written, or not written. Note that
 * blocks containing in-flight change descriptors may not be written at all. */
static void cache_situation_snapshot(uint32_t cache, struct cache_situation * info)
{
	int i;
	struct chdesc * chdesc;
	memset(info, 0, sizeof(*info));
	for(i = 0; i < HASH_TABLE_SIZE; i++)
		for(chdesc = chdescs[i]; chdesc; chdesc = chdesc->next)
		{
			struct cache_block * block;
			/* the local flags will be used to track readiness */
			chdesc->local_flags &= ~CACHE_CHDESC_READY;
			if(chdesc->flags & CHDESC_INFLIGHT)
			{
				assert(chdesc->block);
				block = cache_block_lookup(chdesc->block);
				assert(block);
				if(!(block->local_flags & CACHE_BLOCK_INFLIGHT))
				{
					info->inflight++;
					if(block->chdescs)
						info->dirty_inflight++;
					block->local_flags |= CACHE_BLOCK_INFLIGHT;
				}
			}
			else if(chdesc->owner == cache && chdesc->block)
			{
				block = cache_block_lookup(chdesc->block);
				assert(block);
				if(!(block->local_flags & CACHE_BLOCK_DIRTY))
				{
					info->dirty++;
					if(block->local_flags & CACHE_BLOCK_INFLIGHT)
						info->dirty_inflight++;
					block->local_flags |= CACHE_BLOCK_DIRTY;
				}
				/* use the group_next fields in the chdescs to keep
				 * track of which chdescs are on the block or ready */
				chdesc->group_next[0] = block->chdescs;
				block->chdescs = chdesc;
				block->chdesc_count++;
			}
		}
	for(i = 0; i < HASH_TABLE_SIZE; i++)
	{
		struct cache_block * scan = cache_blocks[i];
		for(; scan; scan = scan->next)
		{
			int change;
			/* nothing is ready on an inflight block */
			if(scan->local_flags & CACHE_BLOCK_INFLIGHT)
				continue;
			do {
				change = 0;
				for(chdesc = scan->chdescs; chdesc; chdesc = chdesc->group_next[0])
				{
					/* already found to be ready */
					if(chdesc->local_flags & CACHE_CHDESC_READY)
						continue;
					if(chdesc_is_ready(chdesc, chdesc->block))
					{
						chdesc->local_flags |= CACHE_CHDESC_READY;
						scan->ready_count++;
						change = 1;
					}
				}
			} while(change);
			if(scan->chdesc_count == scan->ready_count)
			{
				info->full_ready++;
				scan->local_flags |= CACHE_BLOCK_READY;
			}
			else if(scan->ready_count)
			{
				info->half_ready++;
				scan->local_flags |= CACHE_BLOCK_HALFREADY;
			}
			else
			{
				info->not_ready++;
				scan->local_flags |= CACHE_BLOCK_NOTREADY;
			}
		}
	}
	for(i = 0; i < HASH_TABLE_SIZE; i++)
	{
		struct cache_block * scan = cache_blocks[i];
		for(; scan; scan = scan->next)
			for(chdesc = scan->chdescs; chdesc; chdesc = chdesc->group_next[0])
				dblock_update(chdesc, chdesc);
	}
}

/* End cache analysis }}} */

/* Begin commands {{{ */

static int tty = 0;

static int restore_initial_state(int save_applied, int * progress, int * distance, int * percent)
{
	int r;
	struct debug_opcode opcode;
	if(save_applied < opcodes)
		reset_state();
	while(applied < save_applied)
	{
		if(tty)
		{
			int p = *progress * 100 / *distance;
			if(p > *percent)
			{
				*percent = p;
				printf("\e[4D%2d%% ", *percent);
				fflush(stdout);
			}
		}
		r = get_opcode(applied, &opcode);
		if(r < 0)
		{
			printf("%crror %d reading opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
			return r;
		}
		r = apply_opcode(&opcode, NULL, NULL);
		put_opcode(&opcode);
		if(r < 0)
		{
			printf("%crror %d applying opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
			return r;
		}
		applied++;
		(*progress)++;
	}
	return 0;
}

static int command_cache(int argc, const char * argv[])
{
	int progress = 0, distance = 0, percent = -1;
	int r = 0, save_applied = applied;
	struct debug_opcode opcode;
	int caches = 0, finds = 0, looks = 0, writes = 0;
	int alt_finds = 0, alt_looks = 0, alt_writes = 0;
	int ready = 0, half = 0;
	uint32_t cache = 0;
	struct bd * cache_bd = NULL;
	const char * prefix = "";
	const char * status = "";
	struct cache_choice choices;
	
	if(tty)
	{
		/* calculate total distance */
		distance = opcodes;
		if(applied < opcodes)
			distance += applied;
		prefix = "\r\e[K";
		status = "Analyzing cache behavior...     ";
		printf(status);
		fflush(stdout);
	}
	
	/* make sure we start with a clean slate */
	cache_block_clean();
	memset(&choices, 0, sizeof(choices));
	
	/* analyze cache behavior */
	if(applied)
		reset_state();
	while(applied < opcodes)
	{
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
		r = get_opcode(applied, &opcode);
		if(r < 0)
		{
			printf("%crror %d reading opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
			return r;
		}
		if(modules[opcode.module_idx].module == KDB_MODULE_CACHE)
		{
			if(modules[opcode.module_idx].opcodes[opcode.opcode_idx]->opcode == KDB_CACHE_NOTIFY)
			{
				struct bd * bd;
				struct debug_param params[] = {
					{{.name = "cache"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					break;
				assert(params[0].size == 4);
				bd = lookup_bd(params[0].data_4);
				if(bd)
					printf("%sCache detected: %s (0x%08x)", prefix, bd->name, bd->address);
				else
					printf("%sCache detected: 0x%08x", prefix, params[0].data_4);
				if(!caches++)
				{
					printf(" (processing data for this cache)");
					cache = params[0].data_4;
					cache_bd = bd;
				}
				printf("\n");
				if(tty)
				{
					printf("%s\e[4D%2d%% ", status, percent);
					fflush(stdout);
				}
			}
			else if(modules[opcode.module_idx].opcodes[opcode.opcode_idx]->opcode == KDB_CACHE_FINDBLOCK)
			{
				struct debug_param params[] = {
					{{.name = "cache"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					break;
				assert(params[0].size == 4);
				if(params[0].data_4 == cache)
				{
					struct cache_situation info;
					printf("%s", prefix);
					if(choices.choices)
					{
						printf("       LOOK  summary: ready: %5d,     half: %5d, blocked: %5d\n", choices.look_ready, choices.look_half, choices.look_not);
						printf("       WRITE summary: ready: %5d,     half: %5d\n", choices.write_ready, choices.write_half);
						printf("             deps on: ready: %5d,     half: %5d (%6d, %6d)\n", choices.write_rdblocks, choices.write_hdblocks, choices.write_rdeps, choices.write_hdeps);
						ready += choices.write_ready;
						half += choices.write_half;
					}
					/* clean the old cached block info */
					cache_block_clean();
					memset(&choices, 0, sizeof(choices));
					cache_situation_snapshot(cache, &info);
					printf("#%8d: FINDBLOCK; dirty: %5d, inflight: %5d,    both: %5d\n", applied + 1, info.dirty, info.inflight, info.dirty_inflight);
					printf("                      ready: %5d,     half: %5d, blocked: %5d\n", info.full_ready, info.half_ready, info.not_ready);
					if(tty)
					{
						printf("%s\e[4D%2d%% ", status, percent);
						fflush(stdout);
					}
					finds++;
				}
				else
					alt_finds++;
			}
			else if(modules[opcode.module_idx].opcodes[opcode.opcode_idx]->opcode == KDB_CACHE_LOOKBLOCK)
			{
				struct debug_param params[] = {
					{{.name = "cache"}},
					{{.name = "block"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					break;
				assert(params[0].size == 4 && params[1].size == 4);
				if(params[0].data_4 == cache)
				{
					struct cache_block * block = cache_block_lookup(params[1].data_4);
					assert(block);
					if(block->local_flags & CACHE_BLOCK_READY)
						choices.look_ready++;
					else if(block->local_flags & CACHE_BLOCK_HALFREADY)
						choices.look_half++;
					else if(block->local_flags & CACHE_BLOCK_NOTREADY)
						choices.look_not++;
					else
					{
						r = -EINVAL;
						break;
					}
					choices.choices++;
					looks++;
				}
				else
					alt_looks++;
			}
			else if(modules[opcode.module_idx].opcodes[opcode.opcode_idx]->opcode == KDB_CACHE_WRITEBLOCK)
			{
				struct debug_param params[] = {
					{{.name = "cache"}},
					{{.name = "block"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					break;
				assert(params[0].size == 4 && params[1].size == 4);
				if(params[0].data_4 == cache)
				{
					struct cache_block * block = cache_block_lookup(params[1].data_4);
					assert(block);
					if(block->local_flags & CACHE_BLOCK_READY)
					{
						choices.write_ready++;
						choices.write_rdeps += block->dep_count;
						choices.write_rdblocks += block->dblock_count;
					}
					else if(block->local_flags & CACHE_BLOCK_HALFREADY)
					{
						choices.write_half++;
						choices.write_hdeps += block->dep_count;
						choices.write_hdblocks += block->dblock_count;
					}
					else
					{
						r = -EINVAL;
						break;
					}
					choices.choices++;
					writes++;
				}
				else
					alt_writes++;
			}
		}
		r = apply_opcode(&opcode, NULL, NULL);
		put_opcode(&opcode);
		if(r < 0)
		{
			printf("%crror %d applying opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
			return r;
		}
		applied++;
		progress++;
	}
	cache_block_clean();
	if(r < 0)
	{
		printf("%crror %d analyzing opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
		return r;
	}
	if(choices.choices)
	{
		printf("       LOOK  summary: ready: %5d,     half: %5d, blocked: %5d\n", choices.look_ready, choices.look_half, choices.look_not);
		printf("       WRITE summary: ready: %5d,     half: %5d\n", choices.write_ready, choices.write_half);
		printf("             deps on: ready: %5d,     half: %5d (%6d, %6d)\n", choices.write_rdblocks, choices.write_hdblocks, choices.write_rdeps, choices.write_hdeps);
		ready += choices.write_ready;
		half += choices.write_half;
	}
	
	r = restore_initial_state(save_applied, &progress, &distance, &percent);
	if(r < 0)
		return r;
	if(tty)
		printf("\e[4D100%%\n");
	
	printf("Caches: %d, Finds: %d", caches, finds);
	if(alt_finds)
		printf("(+%d)", alt_finds);
	printf(", Looks: %d", looks);
	if(alt_looks)
		printf("(+%d)", alt_looks);
	printf(", Writes: %d", writes);
	if(alt_writes)
		printf("(+%d)", alt_writes);
	printf("\n");
	if(writes)
	{
		printf("Average looks/write: %lg\n", (double) looks / (double) writes);
		printf("Ready blocks written: %d\n", ready);
		printf("Half blocks written: %d\n", half);
	}
	return 0;
}

static void gtk_gui(const char * ps_file);
static int command_line_execute(char * line);
static int command_gui(int argc, const char * argv[])
{
	int child, gui[2];
	child = pipe(gui);
	if(child < 0)
	{
		perror("pipe()");
		return child;
	}
	child = fork();
	if(child < 0)
	{
		close(gui[0]);
		close(gui[1]);
		perror("fork()");
		return child;
	}
	if(!child)
	{
		/* free some memory */
		reset_state();
		close(gui[0]);
		dup2(gui[1], 1);
		close(gui[1]);
		child = open("/dev/null", O_RDWR);
		dup2(child, 0);
		dup2(child, 2);
		close(child);
		gtk_gui((argc > 1) ? argv[1] : NULL);
		exit(0);
		/* just to be sure */
		assert(0);
	}
	else
	{
		char line[64];
		FILE * input = fdopen(gui[0], "r");
		close(gui[1]);
		fgets(line, sizeof(line), input);
		while(!feof(input))
		{
			/* substitute the ps command for the view command if asked */
			if(argc > 1 && (!strcmp(line, "view\n") || !strcmp(line, "view new\n")))
			{
				snprintf(line, sizeof(line), "ps %s", argv[1]);
				line[sizeof(line) - 1] = 0;
			}
			command_line_execute(line);
			fgets(line, sizeof(line), input);
		}
		fclose(input);
		/* child will be collected by collect_children() */
	}
	return 0;
}

static int command_jump(int argc, const char * argv[])
{
	int target, effect = 0;
	int progress = 0, distance, percent = -1;
	if(argc < 2)
	{
		printf("Need an opcode to jump to.");
		return -1;
	}
	target = atoi(argv[1]);
	if(target < 0 || target > opcodes)
	{
		printf("No such opcode.\n");
		return -1;
	}
	printf("Replaying log... %s", tty ? "    " : "");
	fflush(stdout);
	if(target < applied)
		reset_state();
	distance = target - applied;
	while(applied < target)
	{
		int r, opcode_effect;
		struct debug_opcode opcode;
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
		r = get_opcode(applied, &opcode);
		if(r < 0)
		{
			printf("error %d reading opcode %d (%s)\n", -r, applied + 1, strerror(-r));
			return r;
		}
		r = apply_opcode(&opcode, &opcode_effect, NULL);
		put_opcode(&opcode);
		if(r < 0)
		{
			printf("error %d applying opcode %d (%s)\n", -r, applied + 1, strerror(-r));
			return r;
		}
		if(opcode_effect)
			effect = 1;
		applied++;
		progress++;
	}
	printf("%s%d opcode%s OK%s\n", tty ? "\e[4D" : "", applied, (applied == 1) ? "" : "s", effect ? "!" : ", no change.");
	return 0;
}

static int command_list(int argc, const char * argv[])
{
	int i, show_trace = 0, matches = 0;
	int min = 0, max = opcodes - 1;
	const char * prefix = NULL;
	int prefix_length = -1;
	if(argc > 1)
		/* filter by opcode type prefix */
		if(argv[1][0] == 'K')
		{
			prefix = argv[1];
			prefix_length = strlen(prefix);
			argv[1] = argv[0];
			argv = &argv[1];
			argc--;
		}
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
		struct debug_opcode opcode;
		int r = get_opcode(i, &opcode);
		if(r < 0)
		{
			printf("Error %d reading opcode %d (%s)\n", -r, i + 1, strerror(-r));
			return r;
		}
		if(!prefix || !strncmp(prefix, modules[opcode.module_idx].opcodes[opcode.opcode_idx]->name, prefix_length))
		{
			print_opcode(i + 1, &opcode, show_trace);
			matches++;
		}
		put_opcode(&opcode);
	}
	if(prefix)
		printf("Matched %d opcodes.\n", matches);
	return 0;
}

static int command_find(int argc, const char * argv[])
{
	int start = 0, stop = opcodes;
	int save_applied = applied, count;
	int r, extreme = -1, direction;
	struct debug_opcode opcode;
	const char * range = "";
	int progress = 0, distance = 0, percent = -1;
	if(argc < 2 || (strcmp(argv[1], "max") && strcmp(argv[1], "min")))
	{
		printf("Need \"max\" or \"min\" to find.\n");
		return -1;
	}
	if(argc == 4)
	{
		/* search an opcode range */
		start = atoi(argv[2]);
		stop = atoi(argv[3]);
		if(start < 0 || start > stop)
		{
			printf("Invalid range.\n");
			return -1;
		}
		if(stop > opcodes)
			stop = opcodes;
		range = "in range ";
	}
	else if(argc != 2)
	{
		printf("Need a valid opcode range.\n");
		return -1;
	}
	direction = strcmp(argv[1], "max") ? -1 : 1;
	
	if(tty)
	{
		/* calculate total distance */
		distance = stop;
		if(start >= applied)
			distance -= applied;
		distance += applied;
		if(applied >= stop)
			distance -= stop;
		printf("Finding %simum...     ", argv[1]);
		fflush(stdout);
	}
	
	if(start < applied)
		reset_state();
	while(applied < start)
	{
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
		r = get_opcode(applied, &opcode);
		if(r < 0)
		{
			printf("%crror %d reading opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
			return r;
		}
		r = apply_opcode(&opcode, NULL, NULL);
		put_opcode(&opcode);
		if(r < 0)
		{
			printf("%crror %d applying opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
			return r;
		}
		applied++;
		progress++;
	}
	
	/* find the extreme */
	extreme = chdesc_count;
	count = applied;
	while(applied < stop)
	{
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
		r = get_opcode(applied, &opcode);
		if(r < 0)
		{
			printf("%crror %d reading opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
			return r;
		}
		r = apply_opcode(&opcode, NULL, NULL);
		put_opcode(&opcode);
		if(r < 0)
		{
			printf("%crror %d applying opcode %d (%s)\n", tty ? 'e' : 'E', -r, applied + 1, strerror(-r));
			return r;
		}
		applied++;
		progress++;
		if(chdesc_count * direction > extreme * direction)
		{
			extreme = chdesc_count;
			count = applied;
		}
	}
	
	r = restore_initial_state(save_applied, &progress, &distance, &percent);
	if(r < 0)
		return r;
	if(tty)
		printf("\e[4D100%%\n");
	
	printf("The %simum change descriptor count of %d %sfirst occurs at opcode #%d\n", argv[1], extreme, range, count);
	return 0;
}

static const char * lookups[] = {"bd", "block"};
#define LOOKUPS (sizeof(lookups) / sizeof(options[0]))
static int command_lookup(int argc, const char * argv[])
{
	int i, bd = 0;
	if(argc < 2)
	{
		printf("Need an object type and address to look up.\n");
		return -1;
	}
	if(!strcmp(argv[1], "bd"))
		bd = 1;
	else if(strcmp(argv[1], "block"))
	{
		printf("Invalid object type: %s\n", argv[1]);
		return -1;
	}
	if(argc < 3)
	{
		printf("Need a block%s address to look up.\n", bd ? " device" : "");
		return -1;
	}
	for(i = 2; i < argc; i++)
	{
		char * end;
		uint32_t address = strtoul(argv[i], &end, 16);
		if(*end)
			printf("[Info: interpreted %s as 0x%08x.]\n", argv[i], address);
		if(bd)
		{
			struct bd * bd = lookup_bd(address);
			if(bd)
				printf("Block device 0x%08x: %s\n", bd->address, bd->name);
			else
				printf("No such block device: 0x%08x\n", address);
		}
		else
		{
			struct block * block = lookup_block(address);
			if(block)
				printf("Block 0x%08x: #%u\n", block->address, block->number);
			else
				printf("No such block: 0x%08x\n", address);
		}
	}
	return 0;
}

static const char * options[] = {"freelist", "grouping"};
#define OPTIONS (sizeof(options) / sizeof(options[0]))
static int command_option(int argc, const char * argv[])
{
	if(argc < 2)
	{
		printf("Need an option to get or set.\n");
		return -1;
	}
	if(!strcmp(argv[1], "freelist"))
	{
		const char * now = "";
		if(argc > 2)
		{
			if(!strcmp(argv[2], "on"))
				render_free = 1;
			else if(!strcmp(argv[2], "off"))
				render_free = 0;
			else
			{
				printf("Invalid setting: %s\n", argv[2]);
				return -1;
			}
			now = "now ";
		}
		printf("Free list rendering is %so%s\n", now, render_free ? "n" : "ff");
	}
	else if(!strcmp(argv[1], "grouping"))
	{
		int i;
		const char * now = "";
		if(argc > 2)
		{
			for(i = 0; i < GROUPINGS; i++)
				if(!strcmp(argv[2], groupings[i].name))
				{
					current_grouping = groupings[i].type;
					break;
				}
			/* "none" is an unlisted synonym for "off" */
			if(!strcmp(argv[2], "none"))
				current_grouping = OFF;
			else if(i == GROUPINGS)
			{
				printf("Invalid setting: %s\n", argv[2]);
				return -1;
			}
			now = "now ";
			render_block = current_grouping == OFF || current_grouping == OWNER;
			render_owner = current_grouping == OFF || current_grouping == BLOCK;
		}
		printf("Change descriptor grouping is %s%s\n", now, grouping_names[current_grouping]);
	}
	else
	{
		printf("Invalid option: %s\n", argv[1]);
		return -1;
	}
	return 0;
}

static int command_ps(int argc, const char * argv[])
{
	FILE * output;
	char title[256];
	if(argc > 1)
		snprintf(title, sizeof(title), "dot -Tps -o %s", argv[1]);
	else
		snprintf(title, sizeof(title), "dot -Tps");
	output = popen(title, "w");
	if(!output)
	{
		perror("dot");
		return -errno;
	}
	if(applied > 0)
	{
		struct debug_opcode opcode;
		int r = get_opcode(applied - 1, &opcode);
		if(r < 0)
		{
			printf("Error %d reading opcode %d (%s)\n", -r, applied, strerror(-r));
			pclose(output);
			return r;
		}
		r = snprint_opcode(title, sizeof(title), &opcode);
		put_opcode(&opcode);
		assert(r >= 0);
	}
	else
		title[0] = 0;
	render(output, title, 1);
	pclose(output);
	return 0;
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
		struct debug_opcode opcode;
		int r = get_opcode(applied - 1, &opcode);
		if(r < 0)
		{
			printf("Error %d reading opcode %d (%s)\n", -r, applied, strerror(-r));
			return r;
		}
		r = snprint_opcode(title, sizeof(title), &opcode);
		put_opcode(&opcode);
		assert(r >= 0);
	}
	render(output, title, 1);
	if(argc > 1)
		fclose(output);
	return 0;
}

static int command_reset(int argc, const char * argv[])
{
	reset_state();
	return 0;
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

static void print_chdesc_brief(uint32_t address)
{
	struct chdesc * chdesc = lookup_chdesc(address);
	if(chdesc)
	{
		struct arrow * count;
		int afters = 0, befores = 0;
		for(count = chdesc->afters; count; count = count->next)
			afters++;
		for(count = chdesc->befores; count; count = count->next)
			befores++;
		printf(" 0x%08x, %s, ", chdesc->address, type_names[chdesc->type]);
		if(chdesc->block)
		{
			struct block * block = lookup_block(chdesc->block);
			if(block)
				printf("block #%d, ", block->number);
			else
				printf("block 0x%08x, ", chdesc->block);
		}
		switch(chdesc->type)
		{
			case BIT:
				printf("offset %d, xor 0x%08x, ", chdesc->bit.offset, chdesc->bit.xor);
				break;
			case BYTE:
				printf("offset %d, length %d, ", chdesc->byte.offset, chdesc->byte.length);
				break;
			case NOOP:
				break;
		}
		printf("nafters %d, nbefores %d\n", afters, befores);
	}
	else
		printf(" 0x%08x\n", address);
}

static int command_status(int argc, const char * argv[])
{
	if(argc < 2)
	{
		int arrows = (arrow_count + 1) / 2;
		printf("Debugging %s, read %d opcode%s, applied %d\n", input_name, opcodes, (opcodes == 1) ? "" : "s", applied);
		printf("[Info: %d chdesc%s, %d dependenc%s (%d raw)]\n", chdesc_count, (chdesc_count == 1) ? "" : "s", arrows, (arrows == 1) ? "y" : "ies", arrow_count);
	}
	else
	{
		int i = 1;
		int verbose = 0;
		if(!strcmp(argv[1], "-v"))
		{
			verbose = 1;
			i++;
		}
		else if(!strcmp(argv[1], "-vv") || !strcmp(argv[1], "-V"))
		{
			verbose = 2;
			i++;
		}
		for(; i < argc; i++)
		{
			char * end;
			uint32_t address = strtoul(argv[i], &end, 16);
			struct chdesc * chdesc = lookup_chdesc(address);
			if(*end)
				printf("[Info: interpreted %s as 0x%08x.]\n", argv[i], address);
			if(!chdesc)
			{
				printf("No such chdesc: 0x%08x\n", address);
				continue;
			}
			printf("Chdesc 0x%08x was created by opcode %d\n", chdesc->address, chdesc->opcode);
			if(verbose)
			{
				struct label * label;
				for(label = chdesc->labels; label; label = label->next)
					printf("Label = \"%s\"\n", label->label);
				printf("block address = 0x%08x", chdesc->block);
				if(chdesc->block)
				{
					struct block * block = lookup_block(chdesc->block);
					if(block)
						printf(", number = %u", block->number);
				}
				if(chdesc->owner)
				{
					struct bd * bd = lookup_bd(chdesc->owner);
					if(bd)
						printf(", name = %s", bd->name);
				}
				printf("\nFlags: 0x%08x\n", chdesc->flags);
				if(verbose > 1)
				{
					struct arrow * arrow;
					printf("Afters:\n");
					for(arrow = chdesc->afters; arrow; arrow = arrow->next)
						print_chdesc_brief(arrow->chdesc);
					printf("Befores:\n");
					for(arrow = chdesc->befores; arrow; arrow = arrow->next)
						print_chdesc_brief(arrow->chdesc);
				}
			}
		}
	}
	return 0;
}

static int command_step(int argc, const char * argv[])
{
	int delta = (argc > 1) ? atoi(argv[1]) : 1;
	int target = applied + delta, skippable = 1;
	int effect = 0;
	if(target < 0 || target > opcodes)
	{
		printf("No such opcode.\n");
		return -1;
	}
	printf("Replaying log... ");
	fflush(stdout);
	if(target < applied)
		reset_state();
	while(applied < target || (delta == 1 && skippable))
	{
		struct debug_opcode opcode;
		int opcode_effect, r = get_opcode(applied, &opcode);
		if(r < 0)
		{
			printf("error %d reading opcode %d (%s)\n", -r, applied + 1, strerror(-r));
			return r;
		}
		r = apply_opcode(&opcode, &opcode_effect, &skippable);
		put_opcode(&opcode);
		if(r < 0)
		{
			printf("error %d applying opcode %d (%s)\n", -r, applied + 1, strerror(-r));
			return r;
		}
		if(opcode_effect)
			effect = 1;
		applied++;
	}
	printf("%d opcode%s OK%s\n", applied, (applied == 1) ? "" : "s", effect ? "!" : ", no change.");
	return 0;
}

static int view_child = 0;
static FILE * view_file = NULL;

static void gtk_view(void);
static int command_view(int argc, const char * argv[])
{
	char tempfile[] = "/tmp/kdb-XXXXXX";
	FILE * output;
	char title[256];
	int r, fresh = 0;
	
	/* make a temporary image file name */
	r = mkstemp(tempfile);
	if(r < 0)
	{
		/* r will not be the error */
		printf("Error %d creating image file (%s)\n", errno, strerror(errno));
		return -errno;
	}
	close(r);
	
	/* this section is very similar to the ps command */
	snprintf(title, sizeof(title), "dot -Tpng -o %s", tempfile);
	output = popen(title, "w");
	if(!output)
	{
		perror("dot");
		unlink(tempfile);
		return -errno;
	}
	if(applied > 0)
	{
		struct debug_opcode opcode;
		r = get_opcode(applied - 1, &opcode);
		if(r < 0)
		{
			printf("Error %d reading opcode %d (%s)\n", -r, applied, strerror(-r));
			pclose(output);
			unlink(tempfile);
			return r;
		}
		r = snprint_opcode(title, sizeof(title), &opcode);
		put_opcode(&opcode);
		assert(r >= 0);
	}
	else
		title[0] = 0;
	render(output, title, 0);
	pclose(output);
	
	/* abandon the old window if requested */
	if(view_child && argc > 1 && !strcmp(argv[1], "new"))
	{
		fclose(view_file);
		/* rename the old window */
		kill(view_child, SIGUSR2);
		view_child = 0;
		view_file = NULL;
	}
	/* start a new child if necessary */
	if(!view_child)
	{
		int child, gui[2];
		r = pipe(gui);
		if(r < 0)
		{
			perror("pipe()");
			unlink(tempfile);
			return r;
		}
		child = fork();
		if(child < 0)
		{
			perror("fork()");
			close(gui[0]);
			close(gui[1]);
			unlink(tempfile);
			return child;
		}
		if(!child)
		{
			/* free some memory */
			reset_state();
			close(gui[1]);
			dup2(gui[0], 0);
			close(gui[0]);
			child = open("/dev/null", O_WRONLY);
			dup2(child, 1);
			dup2(child, 2);
			close(child);
			gtk_view();
			exit(0);
			/* just to be sure */
			assert(0);
		}
		else
		{
			view_child = child;
			view_file = fdopen(gui[1], "w");
			close(gui[0]);
			fresh = 1;
		}
	}
	
	/* send the file name and window title */
	fprintf(view_file, "%s\n* Debugging %s, read %d opcodes, applied %d\n", tempfile, input_name, opcodes, applied);
	fflush(view_file);
	/* kick the child */
	if(!fresh)
		kill(view_child, SIGUSR1);
	
	return 0;
}

/* End commands }}} */

/* Begin command line processing {{{ */

static int command_help(int argc, const char * argv[]);
static int command_quit(int argc, const char * argv[]);

struct {
	const char * command;
	const char * help;
	int (*execute)(int argc, const char * argv[]);
	const int child_atomic;
} commands[] = {
	{"cache", "Analyze cache options and decisions.", command_cache, 0},
	{"gui", "Start GUI control panel, optionally rendering to PostScript.", command_gui, 0},
	{"jump", "Jump system state to a specified number of opcodes.", command_jump, 0},
	{"list", "List opcodes in a specified range, or all opcodes by default.", command_list, 0},
	{"find", "Find max or min change descriptor count, optionally in an opcode range.", command_find, 0},
	{"lookup", "Lookup block numbers or block devices by address.", command_lookup, 0},
	{"option", "Get or set rendering options: freelist, grouping.", command_option, 0},
	{"ps", "Render system state to a PostScript file, or standard output by default.", command_ps, 1},
	{"render", "Render system state to a GraphViz dot file, or standard output by default.", command_render, 0},
	{"reset", "Reset system state to 0 opcodes.", command_reset, 0},
	{"run", "Apply all opcodes to system state.", command_run, 0},
	{"status", "Displays system state status.", command_status, 0},
	{"step", "Step system state by a specified number of opcodes, or 1 by default.", command_step, 0},
	{"view", "View system state graphically, optionally in a new window.", command_view, 1},
	{"help", "Displays help.", command_help, 0},
	{"quit", "Quits the program.", command_quit, 1}
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
	if(view_child)
	{
		fclose(view_file);
		/* rename the old window */
		kill(view_child, SIGUSR2);
		view_child = 0;
		view_file = NULL;
	}
	return -EINTR;
}

static char * command_complete(const char * text, int state)
{
	static int index, length;
	static enum {
		COMMAND,
		CHDESC,
		BLOCK,
		BD,
		KDB,
		MAXMIN,
		LOOKUP,
		OPTION,
		GROUPING
	} type = COMMAND;
	static union {
		struct {
			struct chdesc * last;
		} chdesc;
		struct {
			struct block * last;
		} block;
		struct {
			struct bd * next;
		} bd;
		struct {
			int module, opcode;
		} kdb;
	} local;
	if(!state)
	{
		int i, spaces[2] = {-1, -1};
		char * argv[2] = {NULL, NULL};
		/* find the first non-whitespace character */
		for(i = 0; i < rl_point && rl_line_buffer[i] == ' '; i++);
		if(i < rl_point)
		{
			argv[0] = &rl_line_buffer[i];
			/* find the first whitespace character after that */
			for(; i < rl_point && rl_line_buffer[i] != ' '; i++);
			if(i < rl_point)
			{
				spaces[0] = i;
				/* and find another non-whitespace character */
				for(; i < rl_point && rl_line_buffer[i] == ' '; i++);
				if(i < rl_point)
				{
					argv[1] = &rl_line_buffer[i];
					/* and find another whitespace character */
					for(; i < rl_point && rl_line_buffer[i] != ' '; i++);
					if(i < rl_point)
						spaces[1] = i;
				}
			}
		}
		/* complete only commands at the beginning of the line */
		if(spaces[0] == -1)
			type = COMMAND;
		else
		{
			if(!strncmp(argv[0], "status ", 7))
			{
				type = CHDESC;
				local.chdesc.last = NULL;
			}
			else if(!strncmp(argv[0], "list ", 5))
			{
				type = KDB;
				local.kdb.module = 0;
				local.kdb.opcode = 0;
			}
			else if(!strncmp(argv[0], "find ", 5))
				type = MAXMIN;
			else if(!strncmp(argv[0], "lookup ", 7))
			{
				if(spaces[1] == -1)
					type = LOOKUP;
				else if(!strncmp(argv[1], "bd ", 3))
				{
					type = BD;
					local.bd.next = bds;
				}
				else if(!strncmp(argv[1], "block ", 6))
				{
					type = BLOCK;
					local.block.last = NULL;
				}
				else
					return NULL;
			}
			else if(!strncmp(argv[0], "option ", 7))
			{
				if(spaces[1] == -1)
					type = OPTION;
				else if(!strncmp(argv[1], "grouping ", 9))
					type = GROUPING;
				else
					return NULL;
			}
			else
				return NULL;
		}
		index = 0;
		length = strlen(text);
	}
	switch(type)
	{
		case COMMAND:
			for(; index < COMMAND_COUNT; index++)
				if(!strncmp(commands[index].command, text, length))
					return strdup(commands[index++].command);
			break;
		case CHDESC:
		{
			char name[11];
			do {
				while(!local.chdesc.last && index < HASH_TABLE_SIZE)
					local.chdesc.last = chdescs[index++];
				for(; local.chdesc.last; local.chdesc.last = local.chdesc.last->next)
				{
					sprintf(name, "0x%08x", local.chdesc.last->address);
					if(!strncmp(name, text, length))
					{
						local.chdesc.last = local.chdesc.last->next;
						return strdup(name);
					}
					if(local.chdesc.last->address < 0x10000000)
					{
						sprintf(name, "0x%x", local.chdesc.last->address);
						if(!strncmp(name, text, length))
						{
							sprintf(name, "0x%08x", local.chdesc.last->address);
							local.chdesc.last = local.chdesc.last->next;
							return strdup(name);
						}
					}
				}
			} while(index < HASH_TABLE_SIZE);
			break;
		}
		case BLOCK:
		{
			char name[11];
			do {
				while(!local.block.last && index < HASH_TABLE_SIZE)
					local.block.last = blocks[index++];
				for(; local.block.last; local.block.last = local.block.last->next)
				{
					sprintf(name, "0x%08x", local.block.last->address);
					if(!strncmp(name, text, length))
					{
						local.block.last = local.block.last->next;
						return strdup(name);
					}
					if(local.block.last->address < 0x10000000)
					{
						sprintf(name, "0x%x", local.block.last->address);
						if(!strncmp(name, text, length))
						{
							sprintf(name, "0x%08x", local.block.last->address);
							local.block.last = local.block.last->next;
							return strdup(name);
						}
					}
				}
			} while(index < HASH_TABLE_SIZE);
			break;
		}
		case BD:
		{
			char name[11];
			for(; local.bd.next; local.bd.next = local.bd.next->next)
			{
				sprintf(name, "0x%08x", local.bd.next->address);
				if(!strncmp(name, text, length))
				{
					local.bd.next = local.bd.next->next;
					return strdup(name);
				}
				if(local.bd.next->address < 0x10000000)
				{
					sprintf(name, "0x%x", local.bd.next->address);
					if(!strncmp(name, text, length))
					{
						sprintf(name, "0x%08x", local.bd.next->address);
						local.bd.next = local.bd.next->next;
						return strdup(name);
					}
				}
			}
			break;
		}
		case KDB:
			while(modules[local.kdb.module].opcodes)
			{
				while(modules[local.kdb.module].opcodes[local.kdb.opcode]->name)
				{
					const char * name = modules[local.kdb.module].opcodes[local.kdb.opcode++]->name;
					if(!strncmp(name, text, length))
						return strdup(name);
				}
				local.kdb.opcode = 0;
				local.kdb.module++;
			}
			break;
		case MAXMIN:
			switch(index++)
			{
				case 0:
					if(!strncmp("max", text, length))
						return strdup("max");
					index++;
				case 1:
					if(!strncmp("min", text, length))
						return strdup("min");
					index++;
				default:
					break;
			}
			break;
		case LOOKUP:
			while(index < LOOKUPS)
			{
				if(!strncmp(lookups[index], text, length))
					return strdup(lookups[index++]);
				index++;
			}
			break;
		case OPTION:
			while(index < OPTIONS)
			{
				if(!strncmp(options[index], text, length))
					return strdup(options[index++]);
				index++;
			}
			break;
		case GROUPING:
			while(index < GROUPINGS)
			{
				if(!strncmp(groupings[index].name, text, length))
					return strdup(groupings[index++].name);
				index++;
			}
			break;
	}
	return NULL;
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
		{
			int r;
			sigset_t set, old;
			if(commands[i].child_atomic)
			{
				sigemptyset(&set);
				sigaddset(&set, SIGCHLD);
				sigprocmask(SIG_BLOCK, &set, &old);
			}
			r = commands[i].execute(argc, argv);
			if(commands[i].child_atomic)
				sigprocmask(SIG_SETMASK, &old, NULL);
			return r;
		}
	return -ENOENT;
}

/* End command line processing }}} */

/* Begin main {{{ */

static int gtk_argc;
static char ** gtk_argv;

static void collect_children(int number)
{
	int child = waitpid(-1, NULL, WNOHANG);
	while(child > 0)
	{
		/* no more view child */
		if(child == view_child)
		{
			fclose(view_file);
			view_child = 0;
			view_file = NULL;
		}
		child = waitpid(-1, NULL, WNOHANG);
	}
}

int main(int argc, char * argv[])
{
	int r, percent = -1;
	struct stat file;
	off_t offset;
	
	tty = isatty(1);
	signal(SIGCHLD, collect_children);
	signal(SIGPIPE, SIG_IGN);
	
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
	if(input_init(argv[1]) < 0)
	{
		perror(argv[1]);
		return 1;
	}
	
	/* set up argc, argv for GTK */
	gtk_argc = argc - 1;
	gtk_argv = &argv[1];
	argv[1] = argv[0];
	
	printf("Reading debug signature... ");
	fflush(stdout);
	r = read_debug_signature();
	if(r < 0)
	{
		printf("error %d (%s)\n", -r, strerror(-r));
		input_finish();
		return 1;
	}
	else
		printf("OK!\n");
	
	printf("Scanning debugging output... %s", tty ? "    " : "");
	fflush(stdout);
	
	while((offset = input_offset()) != file.st_size)
	{
		r = offset * 100 / file.st_size;
		if(r > percent)
		{
			if(tty)
			{
				percent = r;
				printf("\e[4D%2d%% ", percent);
			}
			else
				while(++percent <= r)
					printf("*");
			fflush(stdout);
		}
		
		r = scan_opcode();
		if(r < 0)
			break;
		add_opcode_offset(offset);
	}
	printf("%s%d opcode%s OK!\n", tty ? "\e[4D" : " ", opcodes, (opcodes == 1) ? "" : "s");
	if(r == -1)
		fprintf(stderr, "Unexpected end of file at offset %lld+%lld\n", offset, input_offset() - offset);
	else if(r < 0)
		fprintf(stderr, "Error %d at file offset %lld+%lld (%s)\n", -r, offset, input_offset() - offset, strerror(-r));
	
	if(opcodes)
	{
#if HASH_PRIME || RANDOM_TEST
		int opcode;
#endif
		printf("[Info: average opcode length is %d bytes]\n", (int) ((offset + opcodes / 2) / opcodes));
		
#if HASH_PRIME
		printf("Reading debugging output... %s", tty ? "    " : "");
		fflush(stdout);
		
		percent = -1;
		for(opcode = 0; opcode < opcodes; opcode++)
		{
			struct debug_opcode debug_opcode;
			r = opcode * 100 / opcodes;
			if(r > percent)
			{
				if(tty)
				{
					percent = r;
					printf("\e[4D%2d%% ", percent);
				}
				else
					while(++percent <= r)
						printf("*");
				fflush(stdout);
			}
			
			r = get_opcode(opcode, &debug_opcode);
			if(r < 0)
				break;
			put_opcode(&debug_opcode);
		}
		printf("%s%d unique strings, %d unique stacks OK!\n", tty ? "\e[4D" : " ", unique_strings, unique_stacks);
#endif
		
#if RANDOM_TEST
		printf("Reading random opcodes... %s", tty ? "    " : "");
		fflush(stdout);
		
		percent = -1;
		for(opcode = 0; opcode < opcodes; opcode++)
		{
			struct debug_opcode debug_opcode;
			r = opcode * 100 / opcodes;
			if(r > percent)
			{
				if(tty)
				{
					percent = r;
					printf("\e[4D%2d%% ", percent);
				}
				else
					while(++percent <= r)
						printf("*");
				fflush(stdout);
			}
			
			r = get_opcode((unsigned int) (rand() * RAND_MAX + rand()) % opcodes, &debug_opcode);
			if(r < 0)
				break;
			put_opcode(&debug_opcode);
		}
		printf("%sOK!\n", tty ? "\e[4D" : " ");
#endif
		
		rl_completion_entry_function = command_complete;
		do {
			int i;
			char * line = readline("debug> ");
			if(!line)
			{
				printf("\n");
				line = strdup("quit");
				assert(line);
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
	
	input_finish();
	
	/* free everything */
	cache_block_clean();
	reset_state();
	while(offsets)
	{
		struct opcode_offsets * old = offsets;
		offsets = old->next;
		free(old->offsets);
		free(old);
	}
	strdup_unique(NULL);
	stkdup_unique(NULL);
	
	return 0;
}

/* End main }}} */

/* Begin GTK interface {{{ */

/* This part is for the GUI */

static gboolean close_gui(GtkWidget * widget, GdkEvent * event, gpointer data)
{
	gtk_main_quit();
	return FALSE;
}

static void click_button(GtkWidget * widget, gpointer data)
{
	printf("%s", (const char *) data);
}

static void gtk_gui(const char * ps_file)
{
	char title[64];
	
	GtkWidget * window;
	GtkWidget * table;
	GtkWidget * button;
	GtkWidget * arrow;
	
	gtk_init(&gtk_argc, &gtk_argv);
	
	if(ps_file)
		snprintf(title, sizeof(title), "Debugger GUI: %s", ps_file);
	else
		snprintf(title, sizeof(title), "Debugger GUI");
	title[sizeof(title) - 1] = 0;
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), title);
	gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
	g_signal_connect(G_OBJECT(window), "delete_event", G_CALLBACK(close_gui), NULL);
	
	table = gtk_table_new(1, 5, TRUE);
	gtk_container_add(GTK_CONTAINER(window), table);
	
	button = gtk_button_new_with_label("   Start   ");
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(click_button), "reset\nview\n");
	gtk_table_attach_defaults(GTK_TABLE(table), button, 0, 1, 0, 1);
	gtk_widget_show(button);
	
	button = gtk_button_new();
	arrow = gtk_arrow_new(GTK_ARROW_LEFT, GTK_SHADOW_OUT);
	gtk_container_add(GTK_CONTAINER(button), arrow);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(click_button), "step -1\nview\n");
	gtk_table_attach_defaults(GTK_TABLE(table), button, 1, 2, 0, 1);
	gtk_widget_show(arrow);
	gtk_widget_show(button);
	
	button = gtk_button_new_with_label("   New   ");
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(click_button), "view new\n");
	gtk_table_attach_defaults(GTK_TABLE(table), button, 2, 3, 0, 1);
	gtk_widget_show(button);
	
	button = gtk_button_new();
	arrow = gtk_arrow_new(GTK_ARROW_RIGHT, GTK_SHADOW_OUT);
	gtk_container_add(GTK_CONTAINER(button), arrow);
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(click_button), "step\nview\n");
	gtk_table_attach_defaults(GTK_TABLE(table), button, 3, 4, 0, 1);
	gtk_widget_show(arrow);
	gtk_widget_show(button);
	
	button = gtk_button_new_with_label("   End   ");
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(click_button), "run\nview\n");
	gtk_table_attach_defaults(GTK_TABLE(table), button, 4, 5, 0, 1);
	gtk_widget_show(button);
	
	gtk_widget_show(table);
	gtk_widget_show(window);
	gtk_main();
}

/* This part is for the view command */

static GtkWidget * view_window = NULL;
static GtkWidget * view_scroll = NULL;
static gint view_width = 0, view_height;
static char view_title[256];

static void view_signal(int number)
{
	if(number == SIGUSR1)
	{
		GtkWidget * image;
		GdkPixbuf * pixbuf;
		GtkWidget * old;
		gint old_width, old_height;
		int i;
		
		fgets(view_title, sizeof(view_title), stdin);
		if(feof(stdin))
			return;
		for(i = 0; view_title[i]; i++)
			if(view_title[i] == '\n')
			{
				view_title[i] = 0;
				break;
			}
		image = gtk_image_new_from_file(view_title);
		pixbuf = gtk_image_get_pixbuf(GTK_IMAGE(image));
		unlink(view_title);
		gtk_window_get_size(GTK_WINDOW(view_window), &old_width, &old_height);
		if(!view_width || (view_width == old_width && view_height == old_height))
		{
			int spacing = 0;
			gtk_widget_style_get(view_scroll, "scrollbar-spacing", &spacing, NULL);
			view_width = gdk_pixbuf_get_width(pixbuf) + ++spacing;
			view_height = gdk_pixbuf_get_height(pixbuf) + spacing;
			gtk_window_resize(GTK_WINDOW(view_window), view_width, view_height);
		}
		old = gtk_bin_get_child(GTK_BIN(view_scroll));
		if(old)
			gtk_container_remove(GTK_CONTAINER(view_scroll), old);
		gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(view_scroll), image);
		gtk_widget_show(image);
		fgets(view_title, sizeof(view_title), stdin);
		if(feof(stdin))
			snprintf(view_title, sizeof(view_title), "* Debugging %s, read %d opcode%s", input_name, opcodes, (opcodes == 1) ? "" : "s");
		else
			for(i = 0; view_title[i]; i++)
				if(view_title[i] == '\n')
				{
					view_title[i] = 0;
					break;
				}
		gtk_window_set_title(GTK_WINDOW(view_window), view_title);
	}
	else if(number == SIGUSR2)
		gtk_window_set_title(GTK_WINDOW(view_window), &view_title[2]);
	else
	{
		/* let a second SIGTERM kill us */
		signal(SIGTERM, SIG_DFL);
		gtk_main_quit();
	}
}

static void gtk_view(void)
{
	gtk_init(&gtk_argc, &gtk_argv);
	
	view_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	g_signal_connect(G_OBJECT(view_window), "delete_event", G_CALLBACK(close_gui), NULL);
	
	view_scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(view_window), view_scroll);
	
	signal(SIGUSR1, view_signal);
	signal(SIGUSR2, view_signal);
	signal(SIGTERM, view_signal);
	
	view_signal(SIGUSR1);
	
	gtk_widget_show(view_scroll);
	gtk_widget_show(view_window);
	gtk_main();
}

/* End GTK interface }}} */
