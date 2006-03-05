typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include "elf.h"


#if defined(_BIG_ENDIAN) || defined(BIG_ENDIAN)
inline uint16_t leswap(uint16_t x) {
	return ((x & 0xFF00) >> 8) | (x << 8);
}
inline int16_t leswap(int16_t x) {
	return ((x & 0xFF00) >> 8) | (x << 8);
}
inline uint32_t leswap(uint32_t x) {
	return ((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8)
		| ((x & 0x0000FF00) << 8) | ((x & 0x000000FF) << 24);
}
inline int32_t leswap(int32_t x) {
	return ((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8)
		| ((x & 0x0000FF00) << 8) | ((x & 0x000000FF) << 24);
}
#elif defined(_LITTLE_ENDIAN) || defined(LITTLE_ENDIA)
inline uint16_t leswap(uint16_t x) {
	return x;
}
inline int16_t leswap(int16_t x) {
	return x;
}
inline uint32_t leswap(uint32_t x) {
	return x;
}
inline int32_t leswap(int32_t x) {
	return x;
}
#else
#error Unknown host endianness
#endif

int
get_elf_section_by_type(char *base, struct Secthdr *sh, uint16_t sh_num, uint32_t type,
								uint16_t *idx)
{
	assert(base && sh && idx);
	type = leswap(type);
	for(uint16_t i=0; i < sh_num; i++)
	{
		if(type == sh[i].sh_type)
		{
			*idx = &sh[i] - sh;
			return 0;
		}
	}
	return -1;
}

void
dump_sym(struct Elf *elf, struct Secthdr *sh)
{
	uint16_t sh_sym_idx;
	if(get_elf_section_by_type((char*)elf, sh, leswap(elf->e_shnum), SHT_SYMTBL,
										&sh_sym_idx)) {
		fprintf(stderr, "get_elf_section_by_type() failed\n");
		return;
	}

	struct Secthdr *sh_sym = &sh[sh_sym_idx];
	char *base = (char*) elf;
	char *symtbl_begin = (char*) (base         + leswap(sh_sym->sh_offset));
	char *symtbl_end   = (char*) (symtbl_begin + leswap(sh_sym->sh_size));

	for(char *c = symtbl_begin; c < symtbl_end; c++) {
		printf("%c", *c);
	}
}

void
dump_symstr(struct Elf *elf, struct Secthdr *sh)
{
	uint16_t sh_sym_idx, sh_symstr_idx;
	if(get_elf_section_by_type((char*)elf, sh, leswap(elf->e_shnum), SHT_SYMTBL,
										&sh_sym_idx))
	{
		fprintf(stderr, "get_elf_section_by_type() failed\n");
		return;
	}
	sh_symstr_idx = leswap(sh[sh_sym_idx].sh_link);
	struct Secthdr *sh_symstr = &sh[sh_symstr_idx];
	char *base = (char*) elf;
	char *strtbl_begin = (char*) (base       + leswap(sh_symstr->sh_offset));
	char *strtbl_end   = (char*) (strtbl_begin + leswap(sh_symstr->sh_size));

	for(char *c = strtbl_begin; c < strtbl_end; c++)
		printf("%c", *c);
}

void
print_usage(char *cmd)
{
	fprintf(stderr, "%s: <-sym|-symstr> <elf_file>\n", cmd);
}

int
main(int argc, char **argv)
{
	if(argc != 3)
	{
		print_usage(argv[0]);
		return -1;
	}

	char *section_name = argv[1];
	char *filename = argv[2];

	int bin_fd;
	bin_fd = open(filename, O_RDONLY);
	if(bin_fd < 0)
	{
		fprintf(stderr, "open failed\n");
		return -1;
	}

	struct stat bin_stat;
	fstat(bin_fd, &bin_stat);
	size_t bin_len = bin_stat.st_size;
	char *bin = (char*) mmap(NULL, bin_len, PROT_READ, MAP_PRIVATE, bin_fd, 0);
	if(MAP_FAILED == bin)
	{
		fprintf(stderr, "unable to mmap file\n");
		return -1;
	}

	struct Elf *elf = (struct Elf*) bin;
	if(leswap(ELF_MAGIC) != elf->e_magic)
	{
		fprintf(stderr, "no elf magic\n");
		return -1;
	}

	struct Secthdr *sh = (struct Secthdr*) (bin + leswap(elf->e_shoff));
	if(!strcmp("-sym", section_name))
	{
		dump_sym(elf, sh);
	}
	else if(!strcmp("-symstr", section_name))
	{
		dump_symstr(elf, sh);
	}
	else
	{
		print_usage(argv[0]);
		return -1;
	}

	if(munmap(bin, bin_len))
	{
		fprintf(stderr, "unable to munmap file, continuing\n");
		return -1;
	}

	if(close(bin_fd))
		fprintf(stderr, "error closing file, continuing\n");
	return 0;
}
