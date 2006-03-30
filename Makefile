ifneq ($(BUILD),)

# Just pass the target to the appropriate other Makefile
%: Makefile.$(BUILD)
	$(MAKE) -f Makefile.$(BUILD) $@

else

# Special rules that do something for all Makefiles
.PHONY: kudos user kernel clean fsclean

all: kudos user kernel

kudos user kernel:
	$(MAKE) -f Makefile.$@

clean:
	$(MAKE) -f Makefile.kudos clean
	$(MAKE) -f Makefile.user clean
	$(MAKE) -f Makefile.kernel clean
	rm -rf obj/

fsclean:
	$(MAKE) -f Makefile.kudos fsclean
	$(MAKE) -f Makefile.user fsclean

endif
