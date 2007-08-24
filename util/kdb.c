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
#include "fscore/debug_opcode.h"

#define CONSTANTS_ONLY 1
#include "fscore/patch.h"
#include "fscore/bdesc.h"

/* Set HASH_PRIME to do an extra pass over the input file to prime the string
 * and stack hash tables, and to report on the result. */
#define HASH_PRIME 0

/* Set RANDOM_TEST to do a sequence of random opcode reads after loading an
 * input file to test the speed of non-sequential access. */
#define RANDOM_TEST 0

#define HISTORY_FILE ".kdb_history"

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
static uint32_t initial_timestamp;

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
	if(debug_rev != 4258 || debug_opcode_rev != 4260)
		return -EPROTO;
	
	r = read_lit_32(&initial_timestamp);
	if(r < 0)
		return r;
	
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
	uint32_t timestamp;
	const char * file;
	uint32_t line;
	const char * function;
	uint16_t module, opcode, zero;
	uint32_t stack;
	r = read_lit_32(&timestamp);
	if(r < 0)
		return r;
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
	uint32_t timestamp;
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
	r = read_lit_32(&debug_opcode->timestamp);
	if(r < 0)
		return r;
	debug_opcode->timestamp -= initial_timestamp;
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
	uint32_t patch;
	struct arrow * next;
};

struct label {
	const char * label;
	int count;
	struct label * next;
};

struct patch {
	uint32_t address;
	int opcode;
	uint32_t owner, block;
	enum {BIT, BYTE, EMPTY} type;
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
	struct patch * next;
	/* groups */
	struct patch * group_next[2];
};

static const char * type_names[] = {[BIT] = "BIT", [BYTE] = "BYTE", [EMPTY] = "EMPTY"};

static struct bd * bds = NULL;
static struct block * blocks[HASH_TABLE_SIZE];
static struct patch * patchs[HASH_TABLE_SIZE];
static uint32_t patch_free_head = 0;
static int patch_count = 0;
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
		while(patchs[i])
		{
			struct patch * old = patchs[i];
			patchs[i] = old->next;
			free_arrows(&old->befores);
			free_arrows(&old->afters);
			free_labels(&old->labels);
			free(old);
		}
	patch_count = 0;
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

static struct patch * lookup_patch(uint32_t address)
{
	struct patch * scan;
	int index = address % HASH_TABLE_SIZE;
	for(scan = patchs[index]; scan; scan = scan->next)
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

static struct patch * _patch_create(uint32_t address, uint32_t owner)
{
	int index = address % HASH_TABLE_SIZE;
	struct patch * patch = malloc(sizeof(*patch));
	if(!patch)
		return NULL;
	patch->address = address;
	patch->opcode = applied + 1;
	patch->owner = owner;
	patch->flags = 0;
	patch->local_flags = 0;
	patch->weak_refs = NULL;
	patch->befores = NULL;
	patch->afters = NULL;
	patch->labels = NULL;
	patch->free_prev = 0;
	patch->free_next = 0;
	patch->next = patchs[index];
	patchs[index] = patch;
	patch_count++;
	return patch;
}

static struct patch * patch_create_bit(uint32_t address, uint32_t owner, uint32_t block, uint16_t offset, uint32_t xor)
{
	struct patch * patch = _patch_create(address, owner);
	if(!patch)
		return NULL;
	patch->block = block;
	patch->type = BIT;
	patch->bit.offset = offset;
	patch->bit.xor = xor;
	return patch;
}

static struct patch * patch_create_byte(uint32_t address, uint32_t owner, uint32_t block, uint16_t offset, uint16_t length)
{
	struct patch * patch = _patch_create(address, owner);
	if(!patch)
		return NULL;
	patch->block = block;
	patch->type = BYTE;
	patch->byte.offset = offset;
	patch->byte.length = length;
	return patch;
}

static struct patch * patch_create_empty(uint32_t address, uint32_t owner)
{
	struct patch * patch = _patch_create(address, owner);
	if(!patch)
		return NULL;
	patch->block = 0;
	patch->type = EMPTY;
	return patch;
}

static int patch_add_weak(struct patch * patch, uint32_t location)
{
	struct weak * weak = malloc(sizeof(*weak));
	if(!weak)
		return -ENOMEM;
	weak->location = location;
	weak->next = patch->weak_refs;
	patch->weak_refs = weak;
	return 0;
}

static int patch_rem_weak(struct patch * patch, uint32_t location)
{
	struct weak ** point;
	for(point = &patch->weak_refs; *point; point = &(*point)->next)
		if((*point)->location == location)
		{
			struct weak * old = *point;
			*point = old->next;
			free(old);
			return 0;
		}
	return -ENOENT;
}

static int patch_add_label(struct patch * patch, const char * label)
{
	struct label * scan;
	for(scan = patch->labels; scan; scan = scan->next)
		if(!strcmp(scan->label, label))
		{
			scan->count++;
			return 0;
		}
	scan = malloc(sizeof(*scan));
	if(!scan)
		return -ENOMEM;
	scan->label = label;
	scan->count = 1;
	scan->next = patch->labels;
	patch->labels = scan;
	return 0;
}

static int patch_add_before(struct patch * after, uint32_t before)
{
	struct arrow * arrow = malloc(sizeof(*arrow));
	if(!arrow)
		return -ENOMEM;
	arrow->patch = before;
	arrow->next = after->befores;
	after->befores = arrow;
	arrow_count++;
	return 0;
}

static int patch_add_after(struct patch * before, uint32_t after)
{
	struct arrow * arrow = malloc(sizeof(*arrow));
	if(!arrow)
		return -ENOMEM;
	arrow->patch = after;
	arrow->next = before->afters;
	before->afters = arrow;
	arrow_count++;
	return 0;
}

static int patch_rem_before(struct patch * after, uint32_t before)
{
	struct arrow ** point;
	for(point = &after->befores; *point; point = &(*point)->next)
		if((*point)->patch == before)
		{
			struct arrow * old = *point;
			*point = old->next;
			free(old);
			arrow_count--;
			return 0;
		}
	return -ENOENT;
}

static int patch_rem_after(struct patch * before, uint32_t after)
{
	struct arrow ** point;
	for(point = &before->afters; *point; point = &(*point)->next)
		if((*point)->patch == after)
		{
			struct arrow * old = *point;
			*point = old->next;
			free(old);
			arrow_count--;
			return 0;
		}
	return -ENOENT;
}

static int patch_destroy(uint32_t address)
{
	int index = address % HASH_TABLE_SIZE;
	struct patch ** point;
	for(point = &patchs[index]; *point; point = &(*point)->next)
		if((*point)->address == address)
		{
			struct patch * old = *point;
			*point = old->next;
			free_arrows(&old->befores);
			free_arrows(&old->afters);
			free_labels(&old->labels);
			free(old);
			patch_count--;
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

struct mark {
	uint32_t address;
	int opcode;
	struct mark * next;
};

static struct mark * marks = NULL;

struct group_hash;
struct group {
	uint32_t key;
	struct patch * patchs;
	struct group_hash * sub;
	struct group * next;
};

struct group_hash {
	struct group * hash_table[HASH_TABLE_SIZE];
};

static struct group_hash * groups = NULL;

static struct mark * mark_find(uint32_t address, int opcode)
{
	struct mark * mark;
	for(mark = marks; mark; mark = mark->next)
		if(mark->address == address && mark->opcode == opcode)
			break;
	return mark;
}

static int mark_add(uint32_t address, int opcode)
{
	struct mark * mark = mark_find(address, opcode);
	if(mark)
		return -EEXIST;
	mark = malloc(sizeof(*mark));
	if(!mark)
		return -ENOMEM;
	mark->address = address;
	mark->opcode = opcode;
	mark->next = marks;
	marks = mark;
	return 0;
}

static int mark_remove(uint32_t address, int opcode)
{
	struct mark ** prev = &marks;
	struct mark * scan = marks;
	for(; scan; prev = &scan->next, scan = scan->next)
		if(scan->address == address && scan->opcode == opcode)
			break;
	if(!scan)
		return -ENOENT;
	*prev = scan->next;
	free(scan);
	return 0;
}

static int mark_remove_index(int index)
{
	struct mark ** prev = &marks;
	struct mark * scan = marks;
	if(index < 0)
		return -EINVAL;
	for(; scan; prev = &scan->next, scan = scan->next)
		if(--index < 0)
			break;
	if(!scan)
		return -ENOENT;
	*prev = scan->next;
	free(scan);
	return 0;
}

static struct group * patch_group_key(struct group_hash * hash, uint32_t key, int level, struct patch * patch)
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
		group->patchs = NULL;
		group->sub = NULL;
		group->next = hash->hash_table[index];
		hash->hash_table[index] = group;
	}
	patch->group_next[level] = group->patchs;
	group->patchs = patch;
	return group;
}

static int patch_group(struct patch * patch)
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
		key = patch->block;
	else if(current_grouping == OWNER || current_grouping == OWNER_BLOCK)
		key = patch->owner;
	group = patch_group_key(groups, key, 0, patch);
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
			key = patch->owner;
		else if(current_grouping == OWNER_BLOCK)
			key = patch->block;
		group = patch_group_key(group->sub, key, 1, patch);
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
		/* actually list the patchs */
		struct patch * patch;
		for(patch = group->patchs; patch; patch = patch->group_next[level])
			fprintf(output, "\"ch0x%08x-hc%p\"\n", patch->address, (void *) patch);
	}
	return !!group->key;
}

static void render_groups(FILE * output)
{
	int i;
	if(current_grouping == OFF || !groups)
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

static void render_block_owner(FILE * output, struct patch * patch)
{
	if(patch->block && render_block)
	{
		struct block * block = lookup_block(patch->block);
		if(block)
			fprintf(output, "\\n#%d (0x%08x)", block->number, block->address);
		else
			fprintf(output, "\\non 0x%08x", patch->block);
	}
	if(patch->owner && render_owner)
	{
		struct bd * bd = lookup_bd(patch->owner);
		if(bd)
			fprintf(output, "\\n%s", bd->name);
		else
			fprintf(output, "\\nat 0x%08x", patch->owner);
	}
}

static void render_patch(FILE * output, struct patch * patch, int render_free)
{
	struct label * label;
	struct arrow * arrow;
	struct weak * weak;
	struct mark * mark;
	
	fprintf(output, "\"ch0x%08x-hc%p\" [label=\"0x%08x", patch->address, (void *) patch, patch->address);
	for(label = patch->labels; label; label = label->next)
		if(label->count > 1)
			fprintf(output, "\\n\\\"%s\\\" (x%d)", label->label, label->count);
		else
			fprintf(output, "\\n\\\"%s\\\"", label->label);
	mark = mark_find(patch->address, patch->opcode);
	switch(patch->type)
	{
		case EMPTY:
			render_block_owner(output, patch);
			if(mark)
				fprintf(output, "\",fillcolor=orange,style=\"filled");
			else
				fprintf(output, "\",style=\"");
			break;
		case BIT:
			fprintf(output, "\\n[%d:0x%08x]", patch->bit.offset, patch->bit.xor);
			render_block_owner(output, patch);
			fprintf(output, "\",fillcolor=%s,style=\"filled", mark ? "orange" : "springgreen1");
			break;
		case BYTE:
			fprintf(output, "\\n[%d:%d]", patch->byte.offset, patch->byte.length);
			render_block_owner(output, patch);
			fprintf(output, "\",fillcolor=%s,style=\"filled", mark ? "orange" : "slateblue1");
			break;
	}
	if(patch->flags & PATCH_ROLLBACK)
		fprintf(output, ",dashed,bold");
	if(patch->flags & PATCH_MARKED)
		fprintf(output, ",bold\",color=red");
	else
		fprintf(output, "\"");
	if(patch->flags & PATCH_FREEING)
		fprintf(output, ",fontcolor=red");
	else if(patch->flags & PATCH_WRITTEN)
		fprintf(output, ",fontcolor=blue");
	fprintf(output, "]\n");
	
	for(arrow = patch->befores; arrow; arrow = arrow->next)
	{
		struct patch * before = lookup_patch(arrow->patch);
		if(before)
			fprintf(output, "\"ch0x%08x-hc%p\" -> \"ch0x%08x-hc%p\" [color=black]\n", patch->address, (void *) patch, before->address, (void *) before);
	}
	for(arrow = patch->afters; arrow; arrow = arrow->next)
	{
		struct patch * after = lookup_patch(arrow->patch);
		if(after)
			fprintf(output, "\"ch0x%08x-hc%p\" -> \"ch0x%08x-hc%p\" [color=gray]\n", after->address, (void *) after, patch->address, (void *) patch);
	}
	for(weak = patch->weak_refs; weak; weak = weak->next)
	{
		fprintf(output, "\"0x%08x\" [shape=box,fillcolor=yellow,style=filled]\n", weak->location);
		fprintf(output, "\"0x%08x\" -> \"ch0x%08x-hc%p\" [color=green]\n", weak->location, patch->address, (void *) patch);
	}
	if(patch->free_prev)
	{
		struct patch * prev = lookup_patch(patch->free_prev);
		if(prev)
			fprintf(output, "\"ch0x%08x-hc%p\" -> \"ch0x%08x-hc%p\" [color=orange]\n", prev->address, (void *) prev, patch->address, (void *) patch);
	}
	if(patch->free_next && render_free)
	{
		struct patch * next = lookup_patch(patch->free_next);
		if(next)
			fprintf(output, "\"ch0x%08x-hc%p\" -> \"ch0x%08x-hc%p\" [color=red]\n", patch->address, (void *) patch, next->address, (void *) next);
	}
}

static void render(FILE * output, const char * title, int landscape)
{
	int i, free = 0;
	
	/* header */
	fprintf(output, "digraph \"debug: %d/%d opcode%s, %s\"\n", applied, opcodes, (opcodes == 1) ? "" : "s", input_name);
	fprintf(output, "{\nnodesep=0.25;\nranksep=0.25;\nfontname=\"Helvetica\";\nfontsize=10;\n");
	if(landscape)
		fprintf(output, "rankdir=LR;\norientation=L;\nsize=\"10,7.5\";\n");
	else
		fprintf(output, "rankdir=LR;\norientation=P;\nsize=\"16,16\";\n");
	fprintf(output, "subgraph clusterAll {\nlabel=\"%s\";\ncolor=white;\n", title);
	fprintf(output, "node [shape=ellipse,color=black,fontname=\"Helvetica\",fontsize=10];\n");
	
	for(i = 0; i < HASH_TABLE_SIZE; i++)
	{
		struct patch * patch;
		for(patch = patchs[i]; patch; patch = patch->next)
		{
			int is_free = patch->address == patch_free_head || patch->free_prev;
			if(is_free)
				free++;
			if(render_free)
			{
				if(!(patch->flags & PATCH_WRITTEN))
				{
					int r = patch_group(patch);
					assert(r >= 0); (void) r;
				}
				render_patch(output, patch, 1);
			}
			else if(patch->address == patch_free_head || !patch->free_prev)
			{
				if(!(patch->flags & PATCH_WRITTEN))
				{
					int r = patch_group(patch);
					assert(r >= 0); (void) r;
				}
				render_patch(output, patch, 0);
			}
		}
	}
	
	if(patch_free_head)
	{
		fprintf(output, "subgraph cluster_free {\ncolor=red;\nstyle=dashed;\n");
		if(render_free)
		{
			struct patch * patch = lookup_patch(patch_free_head);
			fprintf(output, "label=\"Free List\";\n");
			while(patch)
			{
				fprintf(output, "\"ch0x%08x-hc%p\"\n", patch->address, (void *) patch);
				patch = lookup_patch(patch->free_next);
			}
			if(free > 3)
			{
				double ratio = sqrt(free / 1.6) / free;
				int cluster = 0;
				free = 0;
				fprintf(output, "subgraph cluster_align {\nstyle=invis;\n");
				patch = lookup_patch(patch_free_head);
				while(patch)
				{
					free++;
					if(cluster < ratio * free)
					{
						cluster++;
						fprintf(output, "\"ch0x%08x-hc%p\"\n", patch->address, (void *) patch);
					}
					patch = lookup_patch(patch->free_next);
				}
				fprintf(output, "}\n");
			}
		}
		else
		{
			fprintf(output, "label=\"Free Head (+%d)\";\n", free - 1);
			fprintf(output, "\"ch0x%08x-hc%p\"\n", patch_free_head, (void *) lookup_patch(patch_free_head));
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

static int param_patch_int_apply(struct debug_opcode * opcode, const char * name1, const char * name2, int (*apply)(struct patch *, uint32_t))
{
	int r;
	struct patch * patch;
	struct debug_param params[3];
	params[0].name = name1;
	params[1].name = name2;
	params[2].name = NULL;
	r = param_lookup(opcode, params);
	if(r < 0)
		return r;
	assert(params[0].size == 4 && params[1].size == 4);
	patch = lookup_patch(params[0].data_4);
	if(!patch)
		return -EFAULT;
	return apply(patch, params[1].data_4);
}

#ifndef ptrdiff_t
#define ptrdiff_t uintptr_t
#endif
#define field_offset(struct, field) ((ptrdiff_t) &((struct *) NULL)->field)

static int param_patch_set_field(struct debug_opcode * opcode, const char * name1, const char * name2, ptrdiff_t field)
{
	int r;
	struct patch * patch;
	struct debug_param params[3];
	params[0].name = name1;
	params[1].name = name2;
	params[2].name = NULL;
	r = param_lookup(opcode, params);
	if(r < 0)
		return r;
	assert(params[0].size == 4 && params[1].size == 4);
	patch = lookup_patch(params[0].data_4);
	if(!patch)
		return -EFAULT;
	/* looks ugly but it's the only way */
	*(uint32_t *) (((uintptr_t) patch) + field) = params[1].data_4;
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
		case KDB_INFO_PATCH_LABEL:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "label"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == (uint8_t) -1);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			r = patch_add_label(patch, params[1].data_v);
			break;
		}
		
		case KDB_BDESC_ALLOC:
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
		
		case KDB_PATCH_CREATE_EMPTY:
		{
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "owner"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			if(!patch_create_empty(params[0].data_4, params[1].data_4))
				r = -ENOMEM;
			break;
		}
		case KDB_PATCH_CREATE_BIT:
		{
			struct debug_param params[] = {
				{{.name = "patch"}},
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
			if(!patch_create_bit(params[0].data_4, params[2].data_4, params[1].data_4, params[3].data_2, params[4].data_4))
				r = -ENOMEM;
			break;
		}
		case KDB_PATCH_CREATE_BYTE:
		{
			struct debug_param params[] = {
				{{.name = "patch"}},
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
			if(!patch_create_byte(params[0].data_4, params[2].data_4, params[1].data_4, params[3].data_2, params[4].data_2))
				r = -ENOMEM;
			break;
		}
		case KDB_PATCH_CONVERT_EMPTY:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			patch->type = EMPTY;
			break;
		}
		case KDB_PATCH_CONVERT_BIT:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "offset"}},
				{{.name = "xor"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 2 &&
					params[2].size == 4);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			patch->type = BIT;
			patch->bit.offset = params[1].data_2;
			patch->bit.xor = params[2].data_4;
			break;
		}
		case KDB_PATCH_CONVERT_BYTE:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "offset"}},
				{{.name = "length"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 2 &&
					params[2].size == 2);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			patch->type = BYTE;
			patch->byte.offset = params[1].data_2;
			patch->byte.length = params[2].data_2;
			break;
		}
		case KDB_PATCH_REWRITE_BYTE:
			/* nothing */
			break;
		case KDB_PATCH_APPLY:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			patch->flags &= ~PATCH_ROLLBACK;
			break;
		}
		case KDB_PATCH_ROLLBACK:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			patch->flags |= PATCH_ROLLBACK;
			break;
		}
		case KDB_PATCH_SET_FLAGS:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "flags"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			patch->flags |= params[1].data_4;
			break;
		}
		case KDB_PATCH_CLEAR_FLAGS:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "flags"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			patch->flags &= ~params[1].data_4;
			break;
		}
		case KDB_PATCH_DESTROY:
		{
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			r = patch_destroy(params[0].data_4);
			break;
		}
		case KDB_PATCH_ADD_BEFORE:
			r = param_patch_int_apply(opcode, "source", "target", patch_add_before);
			break;
		case KDB_PATCH_ADD_AFTER:
			r = param_patch_int_apply(opcode, "source", "target", patch_add_after);
			break;
		case KDB_PATCH_REM_BEFORE:
			r = param_patch_int_apply(opcode, "source", "target", patch_rem_before);
			break;
		case KDB_PATCH_REM_AFTER:
			r = param_patch_int_apply(opcode, "source", "target", patch_rem_after);
			break;
		case KDB_PATCH_WEAK_RETAIN:
			r = param_patch_int_apply(opcode, "patch", "location", patch_add_weak);
			break;
		case KDB_PATCH_WEAK_FORGET:
			r = param_patch_int_apply(opcode, "patch", "location", patch_rem_weak);
			break;
		case KDB_PATCH_SET_OFFSET:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "offset"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 2);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			if(patch->type == BIT)
				patch->bit.offset = params[1].data_2;
			else if(patch->type == BYTE)
				patch->byte.offset = params[1].data_2;
			else
				r = -ENOMSG;
			break;
		}
		case KDB_PATCH_SET_XOR:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "xor"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			if(patch->type != BIT)
			{
				r = -ENOMSG;
				break;
			}
			patch->bit.xor = params[1].data_4;
			break;
		}
		case KDB_PATCH_SET_LENGTH:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "length"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 2);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			if(patch->type != BYTE)
			{
				r = -ENOMSG;
				break;
			}
			patch->byte.length = params[1].data_2;
			break;
		}
		case KDB_PATCH_SET_BLOCK:
		{
			struct patch * patch;
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = "block"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4 && params[1].size == 4);
			patch = lookup_patch(params[0].data_4);
			if(!patch)
			{
				r = -EFAULT;
				break;
			}
			if(patch->type != BIT && patch->type != BYTE && params[1].data_4)
			{
				r = -ENOMSG;
				break;
			}
			patch->block = params[1].data_4;
			break;
		}
		case KDB_PATCH_SET_OWNER:
			r = param_patch_set_field(opcode, "patch", "owner", field_offset(struct patch, owner));
			break;
		case KDB_PATCH_SET_FREE_PREV:
			r = param_patch_set_field(opcode, "patch", "free_prev", field_offset(struct patch, free_prev));
			break;
		case KDB_PATCH_SET_FREE_NEXT:
			r = param_patch_set_field(opcode, "patch", "free_next", field_offset(struct patch, free_next));
			break;
		case KDB_PATCH_SET_FREE_HEAD:
		{
			struct debug_param params[] = {
				{{.name = "patch"}},
				{{.name = NULL}}
			};
			r = param_lookup(opcode, params);
			if(r < 0)
				break;
			assert(params[0].size == 4);
			patch_free_head = params[0].data_4;
			break;
		}
		
		case KDB_PATCH_SATISFY:
		case KDB_PATCH_WEAK_COLLECT:
		case KDB_PATCH_OVERLAP_ATTACH:
		case KDB_PATCH_OVERLAP_MULTIATTACH:
			/* nothing */
			if(effect)
				*effect = 0;
			break;
		
		case KDB_CACHE_NOTIFY:
			if(skippable)
				*skippable = 1;
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
	printf("#%d @%u %s", number, opcode->timestamp, modules[m].opcodes[o]->name);
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
	if(show_trace && opcode->stack[0])
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

#define CACHE_PATCH_READY    0x01

#define CACHE_BLOCK_DIRTY     0x01
#define CACHE_BLOCK_INFLIGHT  0x02
#define CACHE_BLOCK_READY     0x04
#define CACHE_BLOCK_HALFREADY 0x08
#define CACHE_BLOCK_NOTREADY  0x10

static struct cache_block {
	uint32_t address;
	uint32_t local_flags;
	struct block * block;
	struct patch * patchs;
	int patch_count;
	struct patch * ready;
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
	scan->patchs = NULL;
	scan->patch_count = 0;
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

static int patch_is_ready(struct patch * patch, uint32_t block)
{
	struct arrow * scan;
	for(scan = patch->befores; scan; scan = scan->next)
	{
		struct patch * before = lookup_patch(scan->patch);
		assert(before);
		if(before->type == EMPTY)
		{
			/* recursive */
			if(!patch_is_ready(before, block))
				return 0;
		}
		else if(before->block != block)
			return 0;
		else if(!(before->local_flags & CACHE_PATCH_READY))
			return 0;
	}
	return 1;
}

static void dblock_update(struct patch * depender, struct patch * patch)
{
	if(depender == patch || patch->type == EMPTY)
	{
		struct arrow * scan;
		for(scan = patch->befores; scan; scan = scan->next)
		{
			struct patch * before = lookup_patch(scan->patch);
			assert(before);
			dblock_update(depender, before);
		}
	}
	else if(patch->block != depender->block)
	{
		struct cache_block * block = cache_block_lookup(patch->block);
		block->dep_count++;
		if(block->dblock_last != depender->block)
		{
			block->dblock_count++;
			block->dblock_last = depender->block;
		}
	}
}

/* Look at all patchs, and determine which blocks on the given cache
 * can be completely written, partially written, or not written. Note that
 * blocks containing in-flight patchs may not be written at all. */
static void cache_situation_snapshot(uint32_t cache, struct cache_situation * info)
{
	int i;
	struct patch * patch;
	memset(info, 0, sizeof(*info));
	for(i = 0; i < HASH_TABLE_SIZE; i++)
		for(patch = patchs[i]; patch; patch = patch->next)
		{
			struct cache_block * block;
			/* the local flags will be used to track readiness */
			patch->local_flags &= ~CACHE_PATCH_READY;
			if(patch->flags & PATCH_INFLIGHT)
			{
				assert(patch->block);
				block = cache_block_lookup(patch->block);
				assert(block);
				if(!(block->local_flags & CACHE_BLOCK_INFLIGHT))
				{
					info->inflight++;
					if(block->patchs)
						info->dirty_inflight++;
					block->local_flags |= CACHE_BLOCK_INFLIGHT;
				}
			}
			else if(patch->owner == cache && patch->block)
			{
				block = cache_block_lookup(patch->block);
				assert(block);
				if(!(block->local_flags & CACHE_BLOCK_DIRTY))
				{
					info->dirty++;
					if(block->local_flags & CACHE_BLOCK_INFLIGHT)
						info->dirty_inflight++;
					block->local_flags |= CACHE_BLOCK_DIRTY;
				}
				/* use the group_next fields in the patchs to keep
				 * track of which patchs are on the block or ready */
				patch->group_next[0] = block->patchs;
				block->patchs = patch;
				block->patch_count++;
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
				for(patch = scan->patchs; patch; patch = patch->group_next[0])
				{
					/* already found to be ready */
					if(patch->local_flags & CACHE_PATCH_READY)
						continue;
					if(patch_is_ready(patch, patch->block))
					{
						patch->local_flags |= CACHE_PATCH_READY;
						scan->ready_count++;
						change = 1;
					}
				}
			} while(change);
			if(scan->patch_count == scan->ready_count)
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
			for(patch = scan->patchs; patch; patch = patch->group_next[0])
				dblock_update(patch, patch);
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
	FILE * write_log = NULL;
	
	if(argc > 1)
	{
		write_log = fopen(argv[1], "w");
		if(!write_log)
			perror(argv[1]);
	}
	
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
			if(write_log)
				fclose(write_log);
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
					{{.name = "flags16"}},
					{{.name = NULL}}
				};
				r = param_lookup(&opcode, params);
				if(r < 0)
					break;
				assert(params[0].size == 4 && params[1].size == 4 && params[2].size == 2);
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
					if(write_log)
					{
						const char * note = "";
						if(params[2].data_2 & BDESC_FLAG_BITMAP)
							note = " # Bitmap block";
						if(params[2].data_2 & BDESC_FLAG_DIRENT)
							note = " # Directory block";
						if(params[2].data_2 & BDESC_FLAG_INDIR)
							note = " # Indirect block";
						fprintf(write_log, "%u %u%s\n", opcode.timestamp, block->block->number, note);
					}
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
			if(write_log)
				fclose(write_log);
			return r;
		}
		applied++;
		progress++;
	}
	if(write_log)
		fclose(write_log);
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
	if(!getenv("DISPLAY"))
	{
		printf("No DISPLAY environment variable.\n");
		return -1;
	}
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
	extreme = patch_count;
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
		if(patch_count * direction > extreme * direction)
		{
			extreme = patch_count;
			count = applied;
		}
	}
	
	r = restore_initial_state(save_applied, &progress, &distance, &percent);
	if(r < 0)
		return r;
	if(tty)
		printf("\e[4D100%%\n");
	
	printf("The %simum patch count of %d %sfirst occurs at opcode #%d\n", argv[1], extreme, range, count);
	return 0;
}

static void print_patch_brief(struct patch * patch)
{
	struct arrow * count;
	int afters = 0, befores = 0;
	for(count = patch->afters; count; count = count->next)
		afters++;
	for(count = patch->befores; count; count = count->next)
		befores++;
	printf(" 0x%08x, %s, ", patch->address, type_names[patch->type]);
	if(patch->block)
	{
		struct block * block = lookup_block(patch->block);
		if(block)
			printf("block #%d, ", block->number);
		else
			printf("block 0x%08x, ", patch->block);
	}
	switch(patch->type)
	{
		case BIT:
			printf("offset %d, xor 0x%08x, ", patch->bit.offset, patch->bit.xor);
			break;
		case BYTE:
			printf("offset %d, length %d, ", patch->byte.offset, patch->byte.length);
			break;
		case EMPTY:
			break;
	}
	printf("nafters %d, nbefores %d\n", afters, befores);
}

static const char * lookups[] = {"bd", "block"};
#define LOOKUPS (sizeof(lookups) / sizeof(options[0]))
static int command_lookup(int argc, const char * argv[])
{
	int i, bd = 0, verbose = 0;
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
	else if(argc > 2 && !strcmp(argv[2], "-v"))
		verbose = 1;
	if(argc < 3 + verbose)
	{
		printf("Need a block%s address to look up.\n", bd ? " device" : "");
		return -1;
	}
	for(i = 2 + verbose; i < argc; i++)
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
			{
				printf("Block 0x%08x: #%u\n", block->address, block->number);
				if(verbose)
				{
					int index;
					struct patch * scan;
					printf("Patchs:\n");
					for(index = 0; index < HASH_TABLE_SIZE; index++)
						for(scan = patchs[index]; scan; scan = scan->next)
						{
							struct block * compare = lookup_block(scan->block);
							if(scan->block == block->address)
								print_patch_brief(scan);
							else if(compare && compare->number == block->number)
							{
								printf("(#) ");
								print_patch_brief(scan);
							}
						}
				}
			}
			else
				printf("No such block: 0x%08x\n", address);
		}
	}
	return 0;
}

static int command_mark(int argc, const char * argv[])
{
	int i, mark = 0;
	if(argc < 1 || strcmp(argv[0], "unmark"))
		mark = 1;
	if(argc < 2)
	{
		struct mark * scan;
		i = 0;
		printf("Marked patchs:\n");
		for(scan = marks; scan; scan = scan->next)
			printf("  #%d: 0x%08x from opcode #%d\n", ++i, scan->address, scan->opcode);
		return 0;
	}
	if(!mark && argc > 1 && !strcmp(argv[1], "all"))
	{
		while(marks)
			mark_remove_index(0);
		return 0;
	}
	for(i = 1; i < argc; i++)
	{
		char * end;
		uint32_t address = strtoul(argv[i], &end, 16);
		if(mark)
		{
			int r;
			struct patch * patch = lookup_patch(address);
			if(*end)
				printf("[Info: interpreted %s as 0x%08x.]\n", argv[i], address);
			if(!patch)
			{
				printf("No such patch: 0x%08x\n", address);
				continue;
			}
			r = mark_add(patch->address, patch->opcode);
			if(r == -EEXIST)
			{
				printf("[Info: ignoring duplicate mark 0x%08x:%d.]\n", patch->address, patch->opcode);
				continue;
			}
			else if(r < 0)
				return r;
			printf("Created mark 0x%08x:%d\n", patch->address, patch->opcode);
		}
		else
		{
			int r;
			if(argv[i][0] == '#')
			{
				int index = atoi(&argv[i][1]) - 1;
				r = mark_remove_index(index);
			}
			else if(*end != ':')
			{
				printf("[Info: ignoring invalid mark ID %s.]\n", argv[i]);
				continue;
			}
			else
			{
				int opcode = atoi(&end[1]);
				r = mark_remove(address, opcode);
			}
			if(r == -EINVAL)
			{
				printf("Invalid mark: %s\n", argv[i]);
				continue;
			}
			else if(r == -ENOENT)
			{
				printf("No such mark: %s\n", argv[i]);
				continue;
			}
			else if(r < 0)
				return r;
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
		printf("Patch grouping is %s%s\n", now, grouping_names[current_grouping]);
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
		printf("[Info: %d unique string%s, %d unique stack%s]\n", unique_strings, (unique_strings == 1) ? "" : "s", unique_stacks, (unique_stacks == 1) ? "" : "s");
	return r;
}

static int command_status(int argc, const char * argv[])
{
	if(argc < 2)
	{
		int arrows = (arrow_count + 1) / 2;
		printf("Debugging %s, read %d opcode%s, applied %d\n", input_name, opcodes, (opcodes == 1) ? "" : "s", applied);
		printf("[Info: %d patch%s, %d dependenc%s (%d raw)]\n", patch_count, (patch_count == 1) ? "" : "s", arrows, (arrows == 1) ? "y" : "ies", arrow_count);
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
			struct patch * patch = lookup_patch(address);
			if(*end)
				printf("[Info: interpreted %s as 0x%08x.]\n", argv[i], address);
			if(!patch)
			{
				printf("No such patch: 0x%08x\n", address);
				continue;
			}
			printf("Patch 0x%08x (%s) was created by opcode %d\n", patch->address, type_names[patch->type], patch->opcode);
			if(verbose)
			{
				struct label * label;
				for(label = patch->labels; label; label = label->next)
					printf("Label = \"%s\"\n", label->label);
				printf("block address = 0x%08x", patch->block);
				if(patch->block)
				{
					struct block * block = lookup_block(patch->block);
					if(block)
						printf(", number = %u", block->number);
				}
				if(patch->owner)
				{
					struct bd * bd = lookup_bd(patch->owner);
					if(bd)
						printf(", name = %s", bd->name);
				}
				printf("\nFlags: 0x%08x\n", patch->flags);
				if(verbose > 1)
				{
					struct arrow * arrow;
					printf("Afters:\n");
					for(arrow = patch->afters; arrow; arrow = arrow->next)
					{
						struct patch * after = lookup_patch(arrow->patch);
						if(after)
							print_patch_brief(after);
						else
							printf(" 0x%08x\n", arrow->patch);
					}
					printf("Befores:\n");
					for(arrow = patch->befores; arrow; arrow = arrow->next)
					{
						struct patch * before = lookup_patch(arrow->patch);
						if(before)
							print_patch_brief(before);
						else
							printf(" 0x%08x\n", arrow->patch);
					}
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
	
	if(!getenv("DISPLAY"))
	{
		printf("No DISPLAY environment variable.\n");
		return -1;
	}
	
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
	{"find", "Find max or min patch count, optionally in an opcode range.", command_find, 0},
	{"lookup", "Lookup block numbers or block devices by address.", command_lookup, 0},
	{"mark", "Mark a patch to be highlighted in output.", command_mark, 0},
	{"option", "Get or set rendering options: freelist, grouping.", command_option, 0},
	{"ps", "Render system state to a PostScript file, or standard output by default.", command_ps, 1},
	{"render", "Render system state to a GraphViz dot file, or standard output by default.", command_render, 0},
	{"reset", "Reset system state to 0 opcodes.", command_reset, 0},
	{"run", "Apply all opcodes to system state.", command_run, 0},
	{"status", "Displays system state status.", command_status, 0},
	{"step", "Step system state by a specified number of opcodes, or 1 by default.", command_step, 0},
	{"unmark", "Unmark a patch from being highlighted.", command_mark, 0},
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
		PATCH,
		BLOCK,
		BD,
		KDB,
		MARK,
		MAXMIN,
		LOOKUP,
		OPTION,
		GROUPING
	} type = COMMAND;
	static union {
		struct {
			struct patch * last;
		} patch;
		struct {
			struct block * last;
		} block;
		struct {
			struct bd * next;
		} bd;
		struct {
			int module, opcode;
		} kdb;
		struct {
			struct mark * next;
		} mark;
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
			if(!strncmp(argv[0], "status ", 7) || !strncmp(argv[0], "mark ", 5))
			{
				type = PATCH;
				local.patch.last = NULL;
			}
			else if(!strncmp(argv[0], "list ", 5))
			{
				type = KDB;
				local.kdb.module = 0;
				local.kdb.opcode = 0;
			}
			else if(!strncmp(argv[0], "unmark ", 7))
			{
				type = MARK;
				local.mark.next = marks;
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
		case PATCH:
		{
			char name[11];
			do {
				while(!local.patch.last && index < HASH_TABLE_SIZE)
					local.patch.last = patchs[index++];
				for(; local.patch.last; local.patch.last = local.patch.last->next)
				{
					sprintf(name, "0x%08x", local.patch.last->address);
					if(!strncmp(name, text, length))
					{
						local.patch.last = local.patch.last->next;
						return strdup(name);
					}
					if(local.patch.last->address < 0x10000000)
					{
						sprintf(name, "0x%x", local.patch.last->address);
						if(!strncmp(name, text, length))
						{
							sprintf(name, "0x%08x", local.patch.last->address);
							local.patch.last = local.patch.last->next;
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
		case MARK:
		{
			char name[24];
			for(; local.mark.next; local.mark.next = local.mark.next->next)
			{
				sprintf(name, "0x%08x:%d", local.mark.next->address, local.mark.next->opcode);
				if(!strncmp(name, text, length))
				{
					local.mark.next = local.mark.next->next;
					return strdup(name);
				}
			}
			break;
		}
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
		printf("%s%d unique string%s, %d unique stack%s OK!\n", tty ? "\e[4D" : " ", unique_strings, (unique_strings == 1) ? "" : "s", unique_stacks, (unique_stacks == 1) ? "" : "s");
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
		
		read_history(HISTORY_FILE);
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
		write_history(HISTORY_FILE);
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
