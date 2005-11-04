# unix-user kfs makefile

#
# This makefile system follows the structuring conventions
# recommended by Peter Miller in his excellent paper:
#
#	Recursive Make Considered Harmful
#	http://aegis.sourceforge.net/auug97.pdf
#
BASE_OBJDIR := obj
OBJDIR := $(BASE_OBJDIR)/unix-user
UTILDIR := $(BASE_OBJDIR)/util

ifdef GCCPREFIX
SETTINGGCCPREFIX := true
else
-include conf/gcc.mk
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
	$(UTILDIR)/$(HEX2BIN) \
	$(UTILDIR)/$(KNBD_SERVER) \
	$(UTILDIR)/$(KDB_SERVER) \
	$(UTILDIR)/kdb.jar

# Cross-compiler KFS toolchain
#
# This Makefile could automatically use the cross-compiler toolchain,
# but does not for now because we do not need it.

CC	:= $(GCCPREFIX)gcc -pipe
CPP	:= $(GCCPREFIX)cpp
GCC_LIB := $(shell $(CC) -print-libgcc-file-name)
AS	:= $(GCCPREFIX)as
AR	:= $(GCCPREFIX)ar
LD	:= $(GCCPREFIX)ld
OBJCOPY	:= $(GCCPREFIX)objcopy
OBJDUMP	:= $(GCCPREFIX)objdump
NM	:= $(GCCPREFIX)nm

# Native commands
NCC	:= gcc $(CC_VER) -pipe
NCXX	:= g++ $(CC_VER) -pipe
ANT	:= ant
TAR	:= gtar
PERL	:= perl
CTAGS	:= ctags

# Native command flags
NCFLAGS	:= -Wall -pedantic
NCXXFLAGS	:= $(NCFLAGS)
CTAGSFLAGS	:= --extra=+q --langmap=make:+\(GNUmakefile\)\(KMakefrag\)\(UUMakefrag\).mk

# Compiler flags
CFLAGS	:= $(CFLAGS) $(DEFS) $(LABDEFS) -I$(TOP) -MD -Wall -Wno-format -g
CFLAGS	:= $(CFLAGS) -O2

# Lists that the */UUMakefrag makefile fragments will add to
OBJDIRS :=

# Make sure that 'all' is the first target
all: tags TAGS

# Eliminate default suffix rules
.SUFFIXES:

# Delete target files if there is an error (or make is interrupted)
.DELETE_ON_ERROR:

# make it so that no intermediate .o files are never deleted
.PRECIOUS: %.o \
	$(OBJDIR)/lib/%.o $(OBJDIR)/kfs/%.o $(OBJDIR)/fs/%.o $(OBJDIR)/user/%.o

# Rules for building user object files
USER_CFLAGS := $(CFLAGS) -DKUDOS_USER

$(OBJDIR)/user/%.o: user/%.c
	@echo + cc[USER] $<
	@mkdir -p $(@D)
	$(V)$(CC) $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/lib/%.o: lib/%.c
	@echo + cc[LIB] $<
	@mkdir -p $(@D)
	$(V)$(CC) $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/lib/%.o: lib/%.S
	@echo + as[LIB] $<
	@mkdir -p $(@D)
	$(V)$(CC) $(USER_CFLAGS) -c -o $@ $<

$(OBJDIR)/fs/%.o: fs/%.c
	@echo + cc[FS] $<
	@mkdir -p $(@D)
	$(V)$(CC) $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/kfs/%.o: kfs/%.c
	@echo + cc[KFS] $<
	@mkdir -p $(@D)
	$(V)$(CC) -DKFSD $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<


# Build vi/emacs tag files
# TODO: can we give these targets more correct dependencies
TAGDEPS := $(OBJDIR)/fs/clean-fs.img $(UTILS)
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
conf/gcc.mk:
	echo 'GCCPREFIX=' >conf/gcc.mk
	@f=`grep GCCPREFIX conf/gcc.mk | sed 's/.*=//'`; if echo $$f | grep '^[12]\.' >/dev/null 2>&1; then echo "***" 1>&2; \
	echo "*** Error: Your gcc compiler is too old." 1>&2; \
	echo "*** The labs will only work with gcc-3.0 or later, and are only" 1>&2; \
	echo "*** tested on gcc-3.3 and later." 1>&2; \
	echo "***" 1>&2; exit 1; fi
	@if uname 2>&1 | grep Darwin >/dev/null; then true; else echo LIBUTIL=-lutil >>conf/gcc.mk; fi


# Include UUMakefrags for subdirectories
include boot/UUMakefrag
include lib/UUMakefrag
include user/UUMakefrag
include fs/UUMakefrag
include kfs/UUMakefrag
include util/Makefrag


# For deleting the build
fsclean:
	rm -rf $(OBJDIR)/fs/clean-fs.img $(OBJDIR)/fs/fs.img

clean:
	rm -rf $(BASE_OBJDIR) fsformat.d conf/gcc.mk tags TAGS

realclean: clean
	rm -rf lab$(LAB).tar.gz

distclean: realclean
	rm -rf conf/gcc.mk


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
