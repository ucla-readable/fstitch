#include <inc/stab.h>
#include <inc/string.h>

#include <kern/pmap.h>
#include <kern/env.h>
#include <kern/stabs.h>

typedef struct stab {
	uint32_t  n_strx;  // index into string table of name
	uint8_t   n_type;  // type of symbol
	uint8_t   n_other; // misc info (usually empty)
	uint16_t  n_desc;  // description field
	uintptr_t n_value; // value of symbol
} stab_t;

#ifdef USE_STABS
#define STABS_INFO 0x200000

extern const struct stab_t __STAB_BEGIN__[], __STAB_END__[];
extern const char __STABSTR_BEGIN__[], __STABSTR_END__[];

static void stab_binsearch(const stab_t *stabs, uintptr_t addr, int *lx, int *rx, int type)
{
	int l = *lx, r = *rx;
	
	while (l <= r) {
		int m = (l + r) / 2;
		while (m >= l && stabs[m].n_type != type)
			m--;
		if (m < l)
			l = (l + r) / 2 + 1;
		else if (stabs[m].n_value < addr) {
			*lx = m;
			l = m + 1;
		} else if (stabs[m].n_value > addr) {
			*rx = m;
			r = m - 1;
		} else {
			*lx = m;
			l = m;
			addr++;
		}
	}
}

int stab_eip(uintptr_t addr, eipinfo_t *info)
{
	const stab_t *stabs, *stab_end;
	const char *stabstr, *stabstr_end;
	int lfile, rfile, lfun, rfun, lline, rline;

	info->eip_file = "<unknown>";
	info->eip_line = 0;
	info->eip_fn = "<unknown>";
	info->eip_fnlen = 9;
	info->eip_fnaddr = addr;

	if (addr >= KERNBASE) {
		stabs = (stab_t *) __STAB_BEGIN__;
		stab_end = (stab_t *) __STAB_END__;
		stabstr = (char *)  __STABSTR_BEGIN__;
		stabstr_end = (char *) __STABSTR_END__;
	} else {
		const void **thing = (const void **) STABS_INFO;
		if (check_user_page_access(curenv, thing, 0) < 0)
			return -1;
		stabs = ((const stab_t **) thing)[0];
		stab_end = ((const stab_t **) thing)[1];
		stabstr = ((const char **) thing)[2];
		stabstr_end = ((const char **) thing)[3];
		if (check_user_access(curenv, stabs, stab_end - stabs, 0) < 0
		    || check_user_access(curenv, stabstr, stabstr_end - stabstr, 0) < 0
		    || stabstr_end == stabstr
		    || stabstr_end[-1])
			return -1;
	}

	lfile = 0, rfile = stab_end - stabs - 1;
	stab_binsearch(stabs, addr, &lfile, &rfile, N_SO);
	if (lfile == 0)
		return -1;

	lfun = lfile, rfun = rfile;
	stab_binsearch(stabs, addr, &lfun, &rfun, N_FUN);

	/* At this point we know the function name. */
	if (lfun != lfile) {
		if (stabs[lfun].n_strx < stabstr_end - stabstr)
			info->eip_fn = stabstr + stabs[lfun].n_strx;
		info->eip_fnaddr = stabs[lfun].n_value;
		addr -= info->eip_fnaddr;
		lline = lfun, rline = rfun;
	} else {
		info->eip_fn = info->eip_file;
		info->eip_fnaddr = addr;
		lline = lfun = lfile, rline = rfile;
	}
	info->eip_fnlen = strfind(info->eip_fn, ':') - info->eip_fn;

	/* Search for the line number */
	stab_binsearch(stabs, addr, &lline, &rline, N_SLINE);
	if (lline == 0)
		return -1;

	/* Found line number, store it and search backwards for filename */
	info->eip_line = stabs[lline].n_desc;
	while (lline >= lfile && stabs[lline].n_type != N_SOL && (stabs[lline].n_type != N_SO || !stabs[lline].n_value))
		lline--;
	if (lline >= lfile && stabs[lline].n_strx < stabstr_end - stabstr)
		info->eip_file = stabstr + stabs[lline].n_strx;
	
	return 0;
}
#else
int stab_eip(uintptr_t addr, eipinfo_t *info)
{
	return -E_UNSPECIFIED;
}
#endif
