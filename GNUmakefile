#
# This makefile system follows the structuring conventions
# recommended by Peter Miller in his excellent paper:
#
#	Recursive Make Considered Harmful
#	http://aegis.sourceforge.net/auug97.pdf
#
OBJDIR := obj

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

# Elf symbol and symbol string tables
SYMTBL         := symtbl
SYMSTRTBL      := symstrtbl

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
GCC_LIB := $(shell $(CC) -print-libgcc-file-name)
AS	:= $(GCCPREFIX)as
AR	:= $(GCCPREFIX)ar
LD	:= $(GCCPREFIX)ld
OBJCOPY	:= $(GCCPREFIX)objcopy
OBJDUMP	:= $(GCCPREFIX)objdump
NM	:= $(GCCPREFIX)nm

# Native commands
NCC	:= gcc $(CC_VER) -pipe
NCPP	:= g++ $(CC_VER) -pipe
TAR	:= gtar
PERL	:= perl
CTAGS	:= ctags

# Compiler flags
# Note that -O2 is required for the boot loader to fit within 512 bytes;
# -fno-builtin is required to avoid refs to undefined functions in the kernel.
CFLAGS	:= $(CFLAGS) $(DEFS) $(LABDEFS) -fno-builtin -I$(TOP) -MD -Wall -Wno-format -ggdb
CFLAGS   := $(CFLAGS) -O2
BOOTLOADER_CFLAGS := $(CFLAGS) -DKUDOS_KERNEL

# Linker flags for user programs
ULDFLAGS := -Ttext 0x800020

# Lists that the */Makefrag makefile fragments will add to
OBJDIRS :=

# Make sure that 'all' is the first target
all: tags TAGS

# Eliminate default suffix rules
.SUFFIXES:

# Delete target files if there is an error (or make is interrupted)
.DELETE_ON_ERROR:

# make it so that no intermediate .o files are never deleted
.PRECIOUS: %.o $(OBJDIR)/kern/%.o $(OBJDIR)/boot/%.o $(OBJDIR)/fs/%.o \
	$(OBJDIR)/user/%.o

# inc/net for lwip, inc/ for string.h
LIB_NET_CFLAGS := -I$(TOP)/inc/net/ -I$(TOP)/inc/net/ipv4 -I$(TOP)/inc/

# Rules for building kernel object files
KERN_CFLAGS := $(CFLAGS) -DKUDOS_KERNEL

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
USER_CFLAGS := $(CFLAGS) -DKUDOS_USER

$(OBJDIR)/user/%.o: user/%.c
	@echo + cc[USER] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/user/%.o: lib/%.raw
	@echo + ld[USER] $<
	@mkdir -p $(@D)
	$(V)$(LD) -r -o $@ $(ULDFLAGS) $(LDFLAGS) -b binary $<

$(OBJDIR)/user/net/%.o: lib/net/%.c
	@echo + cc[LIB/NET] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/user/%.o: lib/%.c
	@echo + cc[USER] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/user/%.o: lib/%.S
	@echo + as[USER] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) -c -o $@ $<

$(OBJDIR)/fs/%.o: fs/%.c
	@echo + cc[USER] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<

$(OBJDIR)/kfs/%.o: kfs/%.c
	@echo + cc[USER] $<
	@mkdir -p $(@D)
	$(V)$(CC) -nostdinc $(USER_CFLAGS) $(LIB_NET_CFLAGS) -c -o $@ $<


# Build vi/emacs tag files
# TODO: can we give these targets more correct dependencies
tags: $(OBJDIR)/kern/bochs.img $(OBJDIR)/fs/clean-fs.img $(OBJDIR)/util/$(PTYPAIR) $(OBJDIR)/util/$(ELFDUMP_SYMTAB) $(OBJDIR)/util/$(KNBD_SERVER)
	@echo + ctags [VI]
	$(V)find . -type f \
		| grep -v /CVS/ | grep -v ./obj/ | grep -v ~$ | grep -v ./TAGS | grep -v ./tags \
		| $(CTAGS) -L - --langmap=make:+\(GNUmakefile\)\(Makefrag\).mk
TAGS: $(OBJDIR)/kern/bochs.img $(OBJDIR)/fs/clean-fs.img $(OBJDIR)/util/$(PTYPAIR) $(OBJDIR)/util/$(ELFDUMP_SYMTAB) $(OBJDIR)/util/$(KNBD_SERVER)
	@echo + ctags [EMACS]
	$(V)find . -type f \
		| grep -v /CVS/ | grep -v ./obj/ | grep -v ~$ | grep -v ./TAGS | grep -v ./tags \
		| $(CTAGS) -L - -e --langmap=make:+\(GNUmakefile\)\(Makefrag\).mk


# try to infer the correct GCCPREFIX
conf/gcc.mk:
	@if i386-jos-elf-objdump -i 2>&1 | grep '^elf32-i386$$' >/dev/null 2>&1; \
	then echo 'GCCPREFIX=i386-jos-elf-' >conf/gcc.mk; \
	elif objdump -i 2>&1 | grep '^elf32-i386$$' >/dev/null 2>&1; \
	then echo 'GCCPREFIX=' >conf/gcc.mk; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find an i386-*-elf version of GCC/binutils." 1>&2; \
	echo "*** Is the directory with i386-jos-elf-gcc in your PATH?" 1>&2; \
	echo "*** If your i386-*-elf toolchain is installed with a command" 1>&2; \
	echo "*** prefix other than 'i386-jos-elf-', set your GCCPREFIX" 1>&2; \
	echo "*** environment variable to that prefix and run 'make' again." 1>&2; \
	echo "*** To turn off this error, run 'echo GCCPREFIX= >conf/gcc.mk'." 1>&2; \
	echo "***" 1>&2; exit 1; fi
	@f=`grep GCCPREFIX conf/gcc.mk | sed 's/.*=//'`; if echo $$f | grep '^[12]\.' >/dev/null 2>&1; then echo "***" 1>&2; \
	echo "*** Error: Your gcc compiler is too old." 1>&2; \
	echo "*** The labs will only work with gcc-3.0 or later, and are only" 1>&2; \
	echo "*** tested on gcc-3.3 and later." 1>&2; \
	echo "***" 1>&2; exit 1; fi
	@if uname 2>&1 | grep Darwin >/dev/null; then true; else echo LIBUTIL=-lutil >>conf/gcc.mk; fi


# Include Makefrags for subdirectories
include boot/Makefrag
include kern/Makefrag
include lib/Makefrag
include user/Makefrag
include fs/Makefrag
include kfs/Makefrag
include util/Makefrag


bochs: $(OBJDIR)/kern/bochs.img $(OBJDIR)/fs/fs.img
	bochs-nogui

# For deleting the build
fsclean:
	rm -rf $(OBJDIR)/fs/clean-fs.img $(OBJDIR)/fs/fs.img

clean:
	rm -rf $(OBJDIR) kern/appkernbin.c fsformat.d conf/gcc.mk tags TAGS

realclean: clean
	rm -rf lab$(LAB).tar.gz

distclean: realclean
	rm -rf conf/gcc.mk

grade: $(LABSETUP)grade.sh
	$(V)$(MAKE) clean >/dev/null 2>/dev/null
	$(MAKE) all
	sh $(LABSETUP)grade.sh

HANDIN_CMD = tar cf - . | gzip | uuencode lab$(LAB).tar.gz | mail $(HANDIN_EMAIL)
handin: realclean
	$(HANDIN_CMD)
tarball: realclean
	tar cf - `ls -a | grep -v '^\.*$$' | grep -v '^lab$(LAB)\.tar\.gz'` | gzip > lab$(LAB).tar.gz

# For test runs
run-%:
	$(V)rm -f $(OBJDIR)/kern/init.o $(OBJDIR)/kern/bochs.img $(OBJDIR)/fs/fs.img
	$(V)$(MAKE) "DEFS=-DTEST=_binary_obj_user_$*_start -DTESTSIZE=_binary_obj_user_$*_size" $(OBJDIR)/kern/bochs.img $(OBJDIR)/fs/fs.img
	bochs -q 'display_library: nogui'

xrun-%:
	$(V)rm -f $(OBJDIR)/kern/init.o $(OBJDIR)/kern/bochs.img $(OBJDIR)/fs/fs.img
	$(V)$(MAKE) "DEFS=-DTEST=_binary_obj_user_$*_start -DTESTSIZE=_binary_obj_user_$*_size" $(OBJDIR)/kern/bochs.img $(OBJDIR)/fs/fs.img
	bochs -q


# This magic automatically generates makefile dependencies
# for header files included from C source files we compile,
# and keeps those dependencies up-to-date every time we recompile.
# See 'mergedep.pl' for more information.
$(OBJDIR)/.deps: $(foreach dir, $(OBJDIRS), $(wildcard $(OBJDIR)/$(dir)/*.d))
	@mkdir -p $(@D)
	@$(PERL) mergedep.pl $@ $^

-include $(OBJDIR)/.deps

# Create a patch from ../lab$(LAB).tar.gz.
patch-extract-tarball:
	@test -r ../lab$(LAB).tar.gz || (echo "***" 1>&2; \
	echo "*** Can't find '../lab$(LAB).tar.gz'.  Download it" 1>&2; \
	echo "*** into my parent directory and try again." 1>&2; \
	echo "***" 1>&2; false)
	(gzcat ../lab$(LAB).tar.gz 2>/dev/null || zcat ../lab$(LAB).tar.gz) | tar xf -

patch-check-date: patch-extract-tarball
	@pkgdate=`grep PACKAGEDATE lab$(LAB)/conf/lab.mk | sed 's/.*=//'`; \
	test "$(PACKAGEDATE)" = "$$pkgdate" || (echo "***" 1>&2; \
	echo "*** The ../lab$(LAB).tar.gz tarball was created on $$pkgdate," 1>&2; \
	echo "*** but your work directory was expanded from a tarball created" 1>&2; \
	echo "*** on $(PACKAGEDATE)!  I can't tell the difference" 1>&2; \
	echo "*** between your changes and the changes between the tarballs," 1>&2; \
	echo "*** so I won't create an automatic patch." 1>&2; \
	echo "***" 1>&2; false)

patch.diff: patch-extract-tarball always
	@rm -f patch.diff
	@for f in `cd lab$(LAB) && find . -type f -print`; do \
	if diff -u lab$(LAB)/$$f $$f >patch.diffpart || [ $$f = ./boot/lab.mk ]; then :; else \
	echo "*** $$f differs; appending to patch.diff" 1>&2; \
	echo diff -u lab$(LAB)/$$f $$f >>patch.diff; \
	cat patch.diffpart >>patch.diff; \
	fi; done
	@for f in `find . -name lab$(LAB) -prune -o '(' -name '*.[ch]' -o -name '*.cpp' ')' -print`; do \
	if [ '(' '!' -f lab$(LAB)/$$f ')' -a '(' "$$f" != ./kern/appkernbin.c ')' ]; then \
	echo "*** $$f is new; appending to patch.diff" 1>&2; \
	echo diff -u lab$(LAB)/$$f $$f >>patch.diff; \
	echo '--- lab$(LAB)/'$$f >>patch.diff; \
	echo '+++ '$$f >>patch.diff; \
	echo '@@ -0,0 +1,'`wc -l <$$f | tr -d ' 	'`' @@' >>patch.diff; \
	cat $$f | sed 's/^/+/' >>patch.diff; \
	fi; done
	@test -n patch.diff || echo "*** No differences found" 1>&2
	@rm -rf lab$(LAB) patch.diffpart

patch: patch-check-date patch.diff

apply-patch:
	@test -r patch.diff || (echo "***" 1>&2; \
	echo "*** No 'patch.diff' file found!  Did you remember to" 1>&2; \
	echo "*** run 'make patch'?" 1>&2; \
	echo "***" 1>&2; false)
	patch -p0 <patch.diff

always:
	@:

.PHONY: all always patch apply-patch \
	_patch-extract-tarball _patch-check-date _patch-make-diff \
	handin tarball clean realclean clean-labsetup distclean grade labsetup
