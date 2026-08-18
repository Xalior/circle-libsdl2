# syscallwrap.mk — build the syscall wrappers into a Circle kernel, and wrap
# every symbol they implement.
#
# Include this on the line before Circle's Rules.mk and there is nothing
# else to do — a port whose own code opens files with plain C gets both the
# wrappers (see docs/CORE-SPLIT.md, "What the application needs") and the
# --wrap flags that route to them:
#
#   include /wherever/circle-syscallwrap/syscallwrap.mk
#   include $(CIRCLEHOME)/Rules.mk
#
# It adds its object to OBJS, its directory to INCLUDE, the --wrap flags to
# LDFLAGS, and the rules to build both the object and — where the project
# wants one — its dependency file.
#
# WHY THE LINE BEFORE Rules.mk, AND NOT AFTER. Rules.mk reads OBJS at the
# moment it is included and works out its dependency list from it there and
# then, so a source added afterwards is one make never checks. That build is
# not loud about it: everything compiles, and then a header changes and
# nothing rebuilds. The same position also settles CHECK_DEPS, which a
# project can only have set before Rules.mk, so one instruction covers both.
#
# THE --wrap LIST COMES FROM THE SAME PLACE THE .cpp DOES. The symbols below
# are exactly the symbols syscallwrap.cpp defines a __wrap_ for — one list,
# read twice — so a symbol wrapped with no wrapper behind it, or a wrapper
# for a symbol nobody wrapped, cannot happen.
#
# The compile rule is written out rather than left to Rules.mk's pattern rule
# because a pattern rule only matches sources beside the makefile that
# declares it, and this source is somewhere else by definition.

# This directory, captured before anything else can change MAKEFILE_LIST.
SYSCALLWRAP_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# Where the object goes. A project with a board-scoped or configuration-scoped
# object tree already has OBJDIR and gets the same treatment for this object
# as for its own; one that builds objects beside its sources, as Circle's own
# rules do, gets the same here.
SYSCALLWRAP_OBJDIR ?= $(OBJDIR)
ifeq ($(strip $(SYSCALLWRAP_OBJDIR)),)
SYSCALLWRAP_OBJDIR := .
endif

SYSCALLWRAP_OBJ := $(SYSCALLWRAP_OBJDIR)/syscallwrap.o

# Every consumer gets this whole set, whether or not its own code calls all
# of them. A port that finds one missing adds it here and in the source, once,
# for everybody — which is the point of the component.
SYSCALLWRAP_SYMS = \
	_open _close _read _write _lseek _fstat _stat lstat _unlink _rename \
	_fcntl mkdir chdir getcwd dup dup2 \
	opendir readdir closedir rewinddir \
	_gettimeofday _isatty ftruncate

OBJS    += $(SYSCALLWRAP_OBJ)
INCLUDE += -I $(SYSCALLWRAP_DIR)
LDFLAGS += $(addprefix --wrap=,$(SYSCALLWRAP_SYMS))

$(SYSCALLWRAP_OBJ): $(SYSCALLWRAP_DIR)/syscallwrap.cpp
	@mkdir -p $(dir $@)
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) $(DEPFLAGS) -c -o $@ $<

# Circle's default is to generate dependency files with its own %.d rules, and
# those are relative too, so they miss this source in exactly the same way.
# A project that turned that off and put -MD -MP in its own compile line asks
# for no separate dependency file and must not be given a rule promising one.
#
# Circle's own default is applied here because Rules.mk has not been read yet
# and would otherwise apply it too late for this decision.
CHECK_DEPS ?= 1
ifeq ($(strip $(CHECK_DEPS)),1)
$(SYSCALLWRAP_OBJ:.o=.d): $(SYSCALLWRAP_DIR)/syscallwrap.cpp
	@mkdir -p $(dir $@)
	@$(CPP) $(CPPFLAGS) -M -MG -MT $(SYSCALLWRAP_OBJ) -MT $@ -MF $@ $<
endif
