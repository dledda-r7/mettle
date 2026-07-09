
all: mettle

include make/Makefile.tools
include make/Makefile.common
include make/Makefile.mettle

distclean:
	@rm -fr $(BUILD)

clean:
	@rm -fr $(BUILD)/mettle

ARCHES := $(shell cat ARCHES)

# Create the individual build/clean/dist-clean rules for each arch...
define rules_for_each_arch

$(strip $(1)).build: $(TOOLS)/musl-cross/.unpacked $(ROOT)/mettle/configure
	make TARGET=$(strip $(1))

$(strip $(1)).install:
	make TARGET=$(strip $(1)) install

$(strip $(1)).clean:
	make TARGET=$(strip $(1)) clean

$(strip $(1)).distclean:
	make TARGET=$(strip $(1)) distclean

endef

$(foreach a, $(ARCHES), $(eval $(call rules_for_each_arch, $(strip $(a)))))

all-parallel: $(TOOLS) $(patsubst %,%.build,$(ARCHES))

clean-parallel: $(patsubst %,%.clean,$(ARCHES))

distclean-parallel: $(patsubst %,%.distclean,$(ARCHES))

install-parallel: $(patsubst %,%.install,$(ARCHES))

# LLVM build targets
# Usage: make llvm-all  (builds supported targets with Clang/lld)
#        make x86_64-linux-musl.llvm-build  (single target)
LLVM_ARCHES = x86_64-linux-musl i486-linux-musl aarch64-linux-musl

define llvm_rules_for_each_arch

$(strip $(1)).llvm-build: $(TOOLS)/musl-cross/.unpacked $(ROOT)/mettle/configure
	$(MAKE) TARGET=$(strip $(1)) USE_LLVM=1

$(strip $(1)).llvm-install:
	$(MAKE) TARGET=$(strip $(1)) USE_LLVM=1 install

$(strip $(1)).llvm-clean:
	$(MAKE) TARGET=$(strip $(1)) USE_LLVM=1 clean

$(strip $(1)).llvm-distclean:
	$(MAKE) TARGET=$(strip $(1)) USE_LLVM=1 distclean

endef

$(foreach a, $(LLVM_ARCHES), $(eval $(call llvm_rules_for_each_arch, $(strip $(a)))))

llvm-all: $(TOOLS) $(patsubst %,%.llvm-build,$(LLVM_ARCHES))

llvm-clean: $(patsubst %,%.llvm-clean,$(LLVM_ARCHES))

llvm-distclean: $(patsubst %,%.llvm-distclean,$(LLVM_ARCHES))

llvm-install: $(patsubst %,%.llvm-install,$(LLVM_ARCHES))
