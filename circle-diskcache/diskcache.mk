# diskcache.mk — build the disk cache into a Circle kernel.
#
# Include this on the line before Circle's Rules.mk and there is nothing else
# to do. It finds itself, so the directory can be anywhere:
#
#   include /wherever/circle-diskcache/diskcache.mk
#   include $(CIRCLEHOME)/Rules.mk
#
# It adds its object to OBJS, its directory to INCLUDE, and the rules to build
# both the object and — where the project wants one — its dependency file.
#
# WHY THE LINE BEFORE Rules.mk, AND NOT AFTER. Rules.mk reads OBJS at the
# moment it is included and works out its dependency list from it there and
# then, so a source added afterwards is one make never checks. That build is
# not loud about it: everything compiles, and then a header changes and
# nothing rebuilds. The same position also settles CHECK_DEPS, which a project
# can only have set before Rules.mk, so one instruction covers both.
#
# The compile rule is written out rather than left to Rules.mk's pattern rule
# because a pattern rule only matches sources beside the makefile that
# declares it, and this source is somewhere else by definition.

# This directory, captured before anything else can change MAKEFILE_LIST.
DISKCACHE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# Where the object goes. A project with a board-scoped or configuration-scoped
# object tree already has OBJDIR and gets the same treatment for this object
# as for its own; one that builds objects beside its sources, as Circle's own
# rules do, gets the same here.
DISKCACHE_OBJDIR ?= $(OBJDIR)
ifeq ($(strip $(DISKCACHE_OBJDIR)),)
DISKCACHE_OBJDIR := .
endif

DISKCACHE_OBJ := $(DISKCACHE_OBJDIR)/diskcache.o

OBJS    += $(DISKCACHE_OBJ)
INCLUDE += -I $(DISKCACHE_DIR)

$(DISKCACHE_OBJ): $(DISKCACHE_DIR)/diskcache.cpp
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
$(DISKCACHE_OBJ:.o=.d): $(DISKCACHE_DIR)/diskcache.cpp
	@mkdir -p $(dir $@)
	@$(CPP) $(CPPFLAGS) -M -MG -MT $(DISKCACHE_OBJ) -MT $@ -MF $@ $<
endif
