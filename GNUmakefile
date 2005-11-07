# kudos kfs makefile

#
# This makefile system follows the structuring conventions
# recommended by Peter Miller in his excellent paper:
#
#	Recursive Make Considered Harmful
#	http://aegis.sourceforge.net/auug97.pdf
#
BASE_OBJDIR := obj
OBJDIR := $(BASE_OBJDIR)/kudos
UTILDIR := $(BASE_OBJDIR)/util
GCCCONF := conf/Kgcc.mk

ifdef GCCPREFIX
SETTINGGCCPREFIX := true
else
-include $(GCCCONF)
endif

ifdef LAB
SETTINGLAB := true
else
-include conf/lab.mk
endif

-include conf/env.mk

ifndef SOL
SOL := 0
endif
ifndef LABADJUST
LABADJUST := 0
endif

ifndef LABSETUP
LABSETUP := ./
endif


TOP = .

# Native utilities
ELFDUMP_SYMTAB := elfdump_symtab
PTYPAIR        := ptypair
KNBD_SERVER    := knbd-server
KDB_SERVER     := kdb-server
HEX2BIN        := hex2bin

# Elf symbol and symbol string tables
SYMTBL         := symtbl
SYMSTRTBL      := symstrtbl

UTILS := \
	$(UTILDIR)/$(PTYPAIR) \
	$(UTILDIR)/$(ELFDUMP_SYMTAB) \
	$(UTILDIR)/$(HEX2BIN) \
	$(UTILDIR)/$(KNBD_SERVER) \
	$(UTILDIR)/$(KDB_SERVER) \
	$(UTILDIR)/kdb.jar

# Cross-compiler KudOS toolchain
#
# This Makefile will automatically use the cross-compiler toolchain
# installed as 'i386-jos-elf-*', if one exists.  If the host tools ('gcc',
# 'objdump', and so forth) compile for a 32-bit x86 ELF target, that will
# be detected as well.  If you have the right compiler toolchain installed
# using a different name, set GCCPREFIX explicitly by doing
#
#	make 'GCCPREFIX=i386-jos-elf-' gccsetup
#

CC	:= $(GCCPREFIX)gcc -pipe
CPP	:= $(GCCPREFIX)cpp
GCC_LIB := $(shell $(CC) -print-libgcc-file-name)
AS	:= $(GCCPREFIX)as
AR	:= $(GCCPREFIX)ar
LD	:= $(GCCPREFIX)ld
OBJCOPY	:= $(GCCPREFIX)objcopy
OBJDUMP	:= $(GCCPREFIX)objdump
NM	:= $(GCCPREFIX)nm

ifdef USE_STABS
# Strip nothing for now, but it would be nice to strip the unnecessary symtab
# and strtab sections.
STRIP   := true
CFLAGS  := $(CFLAGS) -DUSE_STABS
LD_CPPFLAGS := $(CPPFLAGS) -DUSE_STABS
else
STRIP	:= $(GCCPREFIX)strip
endif

# Native commands
NCC	:= gcc $(CC_VER) -pipe
NCXX	:= g++ $(CC_VER) -pipe
ANT	:= ant
TAR	:= gtar
PERL	:= perl
CTAGS	:= ctags

# Native command flags
NCFLAGS	:= -Wall -pedantic -DKUTIL
NCXXFLAGS	:= $(NCFLAGS)
CTAGSFLAGS	:= --extra=+q --langmap=make:+\(GNUmakefile\)\(KMakefrag\)\(UUMakefrag\).mk

# Compiler flags
# Note that -O2 is required for the boot loader to fit within 512 bytes;
# -fno-builtin is required to avoid refs to undefined functions in the kernel.
CFLAGS	:= $(CFLAGS) $(DEFS) $(LABDEFS) -fno-builtin -I$(TOP) -I$(TOP)/inc -MD -Wall -Wno-format -gstabs
CFLAGS	:= $(CFLAGS) -O2
BOOTLOADER_CFLAGS := $(CFLAGS) -DKUDOS -DKUDOS_KERNEL

LD_CPPFLAGS := $(LD_CPPFLAGS) -I$(TOP) -traditional-cpp -P -C -undef

# Linker flags for user programs
ULDFLAGS := -T $(OBJDIR)/user/user.ld

# Lists that the */KMakefrag makefile fragments will add to
OBJDIRS :=

# Make sure that 'all' is the first target
all: tags TAGS

# Eliminate default suffix rules
.SUFFIXES:

# Delete target files if there is an error (or make is interrupted)
.DELETE_ON_ERROR:

# make it so that no intermediate .o files are never deleted
.PRECIOUS: %.o $(OBJDIR)/kern/%.o $(OBJDIR)/boot/%.o \
	$(OBJDIR)/lib/%.o $(OBJDIR)/kfs/%.o $(OBJDIR)/fs/%.o $(OBJDIR)/user/%.o


# Rules for building linker scripts and dealing with on/off STABS support
$(OBJDIR)/%.ld: %.ld conf/env.mk
	@echo + cpp $<
	@mkdir -p $(@D)
	$(V)$(CPP) $(LD_CPPFLAGS) $< $@

$(OBJDIR)/kern/stabs.o: conf/env.mk


# inc/net for lwip, inc/ for string.h
LIB_NET_CFLAGS := -I$(TOP)/inc/net/ -I$(TOP)/inc/net/ipv4 -I$(TOP)/inc/ -DKUDOS

# Rules for building kernel object files
KERN_CFLAGS := $(CFLAGS) -DKUDOS -DKUDOS_KERNEL

$(OBJDIR)/kern/%.o: kern/%.c
	@echo + cc $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(KERN_CFLAGS) -c -o $@ $<

$(OBJDIR)/kern/%.o: kern/%.S
	@echo + as $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(KERN_CFLAGS) -c -o $@ $<

$(OBJDIR)/kern/%.o: lib/%.c
	@echo + cc $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(KERN_CFLAGS) -c -o $@ $<

$(OBJDIR)/boot/%.o: boot/%.c
	@echo + cc $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(KERN_CFLAGS) -c -o $@ $<

$(OBJDIR)/boot/%.o: boot/%.S
	@echo + as $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(KERN_CFLAGS) -c -o $@ $<


# Rules for building user object files
USER_CFLAGS := $(CFLAGS) -DKUDOS -DKUDOS_USER

$(OBJDIR)/user/%.o: user/%.c
	@echo + cc[USER] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/lib/%.o: lib/%.raw
	@echo + ld[LIB] $<
	@mkdir -p $(@D)
	$(V)$(LD) -r -o $@ $(ULDFLAGS) $(LDFLAGS) -b binary $<

$(OBJDIR)/lib/net/%.o: lib/net/%.c
	@echo + cc[LIB/NET] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/lib/%.o: lib/libmad/%.c
	@echo + cc[LIBMAD] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/lib/%.o: lib/%.c
	@echo + cc[LIB] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/lib/%.o: lib/%.S
	@echo + as[LIB] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) -c -o $@ $<

$(OBJDIR)/fs/%.o: fs/%.c
	@echo + cc[FS] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/kfs/%.o: kfs/%.c
	@echo + cc[KFS] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc -DKFSD $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<


# Build vi/emacs tag files
# TODO: can we give these targets more correct dependencies
TAGDEPS := $(OBJDIR)/kern/bochs.img $(OBJDIR)/fs/clean-fs.img $(UTILS)
tags: $(TAGDEPS)
	@echo + ctags [VI]
	$(V)find . -type f \
		| grep -v /CVS/ | grep -v ./obj/ | grep -v ~$ | grep -v ./TAGS | grep -v ./tags \
		| $(CTAGS) $(CTAGSFLAGS) -L -
TAGS: $(TAGDEPS)
	@echo + ctags [EMACS]
	$(V)find . -type f \
		| grep -v /CVS/ | grep -v ./obj/ | grep -v ~$ | grep -v ./TAGS | grep -v ./tags \
		| $(CTAGS) $(CTAGSFLAGS) -L - -e 


# try to infer the correct GCCPREFIX
$(GCCCONF):
	@if i386-jos-elf-objdump -i 2>&1 | grep '^elf32-i386$$' >/dev/null 2>&1; \
	then echo 'GCCPREFIX=i386-jos-elf-' >$(GCCCONF); \
	elif objdump -i 2>&1 | grep '^elf32-i386$$' >/dev/null 2>&1; \
	then echo 'GCCPREFIX=' >$(GCCCONF); \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find an i386-*-elf version of GCC/binutils." 1>&2; \
	echo "*** Is the directory with i386-jos-elf-gcc in your PATH?" 1>&2; \
	echo "*** If your i386-*-elf toolchain is installed with a command" 1>&2; \
	echo "*** prefix other than 'i386-jos-elf-', set your GCCPREFIX" 1>&2; \
	echo "*** environment variable to that prefix and run 'make' again." 1>&2; \
	echo "*** To turn off this error, run 'echo GCCPREFIX= >$(GCCCONF)'." 1>&2; \
	echo "***" 1>&2; exit 1; fi
	@f=`grep GCCPREFIX $(GCCCONF) | sed 's/.*=//'`; if echo $$f | grep '^[12]\.' >/dev/null 2>&1; then echo "***" 1>&2; \
	echo "*** Error: Your gcc compiler is too old." 1>&2; \
	echo "*** The labs will only work with gcc-3.0 or later, and are only" 1>&2; \
	echo "*** tested on gcc-3.3 and later." 1>&2; \
	echo "***" 1>&2; exit 1; fi
	@if uname 2>&1 | grep Darwin >/dev/null; then true; else echo LIBUTIL=-lutil >>$(GCCCONF); fi


# Include KMakefrags for subdirectories
include boot/KMakefrag
include kern/KMakefrag
include lib/KMakefrag
include user/KMakefrag
include fs/KMakefrag
include kfs/KMakefrag
include util/Makefrag


# For deleting the build
fsclean:
	rm -rf $(OBJDIR)/fs/clean-fs.img $(OBJDIR)/fs/fs.img

clean:
	rm -rf $(BASE_OBJDIR) fs/.journal kern/appkernbin.c fsformat.d $(GCCCONF) tags TAGS

realclean: clean
	rm -rf lab$(LAB).tar.gz

distclean: realclean
	rm -rf $(GCCCONF)


# This magic automatically generates makefile dependencies
# for header files included from C source files we compile,
# and keeps those dependencies up-to-date every time we recompile.
# See 'mergedep.pl' for more information.
$(OBJDIR)/.deps: $(foreach dir, $(OBJDIRS), $(wildcard $(OBJDIR)/$(dir)/*.d))
	@mkdir -p $(@D)
	@$(PERL) util/mergedep.pl $@ $^

-include $(OBJDIR)/.deps

always:
	@:

.PHONY: all always clean realclean distclean
