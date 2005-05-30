# env.mk - configuration variables for the KudOS lab


# '$(V)' controls whether the lab makefiles print verbose commands (the
# actual shell commands run by Make), as well as the "overview" commands
# (such as '+ cc lib/readline.c').
#
# For overview commands only, the line should read 'V = @'.
# For overview and verbose commands, the line should read 'V ='.
V = @


# '$(HANDIN_EMAIL)' is the email address to which lab handins should be
# sent.
HANDIN_EMAIL = kohler@cs.ucla.edu


# '$(USE_STABS)' controls whether stabs is built in and used for enhanced
# backtraces. Define to enable, do not define to disable.
# Stabs use is optional because the stabs sections substantially increase
# a binary's size.
# NOTE: disabling stabs does not yet work, it looks like we need to find a way
# to change kernel.ld and user.ld, remove the stabs loadable segment, or
# mark the stabs segment as non-loadable.
USE_STABS = 1
