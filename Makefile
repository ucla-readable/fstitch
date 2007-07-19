# Linux kernel KFS Makefile

BASE_OBJDIR := obj
OBJDIR := $(BASE_OBJDIR)/kernel
UTILDIR := $(BASE_OBJDIR)/util

V = @

BIN := kfs/kkfsd.ko $(OBJDIR)/lib/libopgroup.so

ifeq ($(KERNELRELEASE),)
KERNELRELEASE = $(shell uname -r)
endif

ifeq ($(KERNELPATH),)
KERNELPATH = /lib/modules/${KERNELRELEASE}/build
endif

# Commands
CC	:= gcc $(CC_VER) -pipe
TAR	:= tar
PERL	:= perl
CTAGS	:= ctags

# Command flags
CFLAGS	:= -Wall -std=gnu99
CTAGSFLAGS	:= --extra=+q --langmap=make:+\(Makefile\)\(Makefile.user\).mk

# Lists that the */Makefrag makefile fragments will add to
OBJDIRS :=

all: tags TAGS

# Eliminate default suffix rules
.SUFFIXES:

# Delete target files if there is an error (or make is interrupted)
.DELETE_ON_ERROR:

# make it so that no intermediate .o files are never deleted
.PRECIOUS: %.o

user:
	$(MAKE) -f Makefile.user

kfs/kkfsd.ko: always
	@[ ! -f .kernel_version ] || [ "`cat .kernel_version`" == "$(KERNELRELEASE)" ] || $(MAKE) -C $(KERNELPATH) M=$(shell pwd) clean
	@echo "$(KERNELRELEASE)" > .kernel_version
	$(MAKE) -C $(KERNELPATH) M=$(shell pwd) modules
	@[ -f .user ] && $(MAKE) -f Makefile.user || true

install: kfs/kkfsd.ko
	$(MAKE) -C $(KERNELPATH) M=$(shell pwd) modules_install

$(OBJDIR)/lib/libopgroup.so: lib/kernel_opgroup.c
	@echo + cc[LIB] $<
	@mkdir -p $(@D)
	$(V)$(CC) -DKERNEL_USER -I. $(CFLAGS) -std=gnu99 -g -o $@ $< -shared

# Include Makefrags for subdirectories
include fs/Makefrag
include util/Makefrag

# Build vi/emacs tag files
# thetags - for if your source does not build
thetags::
	@echo + ctags [vi]
	$(V)find . -type f \
		| grep -v ./obj/ | grep -v ~$ | grep -v ./TAGS | grep -v ./tags \
		| $(CTAGS) $(CTAGSFLAGS) -L -
	@echo + ctags [emacs]
	$(V)find . -type f \
		| grep -v ./obj/ | grep -v ~$ | grep -v ./TAGS | grep -v ./tags \
		| $(CTAGS) $(CTAGSFLAGS) -L - -e
# TODO: can we give these targets more correct dependencies
TAGDEPS := $(BIN)
tags: $(TAGDEPS)
	@echo + ctags [vi]
	$(V)find . -type f \
		| grep -v ./obj/ | grep -v ~$ | grep -v ./TAGS | grep -v ./tags \
		| $(CTAGS) $(CTAGSFLAGS) -L -
TAGS: $(TAGDEPS)
	@echo + ctags [emacs]
	$(V)find . -type f \
		| grep -v ./obj/ | grep -v ~$ | grep -v ./TAGS | grep -v ./tags \
		| $(CTAGS) $(CTAGSFLAGS) -L - -e

# For deleting the build
fsclean:
	rm -f $(BASE_OBJDIR)/fs/ufs.img $(BASE_OBJDIR)/fs/ext2.img 

clean:
	$(MAKE) -C $(KERNELPATH) M=$(shell pwd) clean
	rm -rf $(BASE_OBJDIR) tags TAGS Module.symvers .kernel_version

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

.PHONY: all always install clean user
