EXTRA_CFLAGS += -I$(src)
EXTRA_CFLAGS += -DFSTITCHD

# TODO: why do these not work for the warning test below?
# (Tested 2006/5/30 on kudos-mm by Frost)
#GCC_VERSION := $(call cc-version)
#ifeq ($(shell echo $(GCC_VERSION) -ge 0304),0)
# The more appropriate "$(call cc-option,foo)" also does not work.
              
GCC_VERSION := $(shell $(CONFIG_SHELL) $(srctree)/scripts/gcc-version.sh $(CC))
ifeq ($(shell [ $(GCC_VERSION) -ge 0304 ] && echo 1 || echo 0),1)
	EXTRA_CFLAGS += -Wno-declaration-after-statement
endif

fstitchobjrel = obj/kernel
fstitchobj = $(src)/$(fstitchobjrel)

obj-m += kfstitchd.o

KFSTITCHD_LIB_OBJS := \
              assert.o \
              vector.o \
              hash_map.o \
              strtol.o \
              sleep.o

KFSTITCHD_FSTITCH_OBJS := \
              bdesc.o \
              block_alloc.o \
              blockman.o \
              bsd_ptable.o \
              debug.o \
              destroy.o \
              fstitchd_init.o \
              fstitchd.o \
              kernel_patchgroup_ops.o \
              kernel_patchgroup_scopes.o \
              kernel_serve.o \
              modman.o \
              patchgroup.o \
              patch.o \
              patch_util.o \
              pc_ptable.o \
              revision.o \
              sched.o \
              sync.o

KFSTITCHD_MODULE_OBJS := \
              block_resizer_bd.o \
              crashsim_bd.o \
              journal_bd.o \
              linux_bd.o \
              loop_bd.o \
              md_bd.o \
              mem_bd.o \
              partition_bd.o \
              unlink_bd.o \
              wb_cache_bd.o \
              wb2_cache_bd.o \
              wbr_cache_bd.o \
              wt_cache_bd.o \
              ext2_lfs.o \
              waffle_lfs.o \
              josfs_lfs.o \
              ufs_lfs.o \
              ufs_alloc_lastpos.o \
              ufs_alloc_linear.o \
              ufs_cg_wb.o \
              ufs_common.o \
              ufs_dirent_linear.o \
              ufs_super_wb.o \
              patchgroup_lfs.o \
              wholedisk_lfs.o \
              devfs_cfs.o \
              icase_cfs.o \
              uhfs_cfs.o

# This file has gone stale, unfortunately
#              file_hiding_cfs.o \

kfstitchd-objs := $(addprefix $(fstitchobjrel)/,$(KFSTITCHD_LIB_OBJS)) \
		$(addprefix $(fstitchobjrel)/,$(KFSTITCHD_FSTITCH_OBJS)) \
		$(addprefix $(fstitchobjrel)/,$(KFSTITCHD_MODULE_OBJS))

$(addprefix $(fstitchobj)/,$(KFSTITCHD_LIB_OBJS)): $(fstitchobj)/%.o : $(src)/lib/%.c
	@test -d $(fstitchobj)/kernel || mkdir -p $(fstitchobj)/kernel
	$(call cmd,force_checksrc)
	$(call if_changed_rule,cc_o_c)

$(addprefix $(fstitchobj)/,$(KFSTITCHD_FSTITCH_OBJS)): $(fstitchobj)/%.o : $(src)/fscore/%.c
	@test -d $(fstitchobj)/kernel || mkdir -p $(fstitchobj)/kernel
	$(call cmd,force_checksrc)
	$(call if_changed_rule,cc_o_c)

$(addprefix $(fstitchobj)/,$(KFSTITCHD_MODULE_OBJS)): $(fstitchobj)/%.o : $(src)/modules/%.c
	@test -d $(fstitchobj)/kernel || mkdir -p $(fstitchobj)/kernel
	$(call cmd,force_checksrc)
	$(call if_changed_rule,cc_o_c)
