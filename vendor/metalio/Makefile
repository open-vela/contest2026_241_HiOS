############################################################################
# vendor/metalio/Makefile
#
# Drivers are compiled via nuttx board package metalio-claw-4.
# This Makefile exists so repo/apps discovery stays happy.
############################################################################

include $(APPDIR)/Make.defs

# No standalone library build — board Make.defs pulls sources.

all:
	@echo "metalio: drivers built via boards/risc-v/esp32p4/metalio-claw-4"

.PHONY: all clean distclean preconfig

clean:
	@:

distclean:
	@:

preconfig:
	@$(Q) touch .kconfig
