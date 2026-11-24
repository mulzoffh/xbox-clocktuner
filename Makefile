XBE_TITLE = ClockTuner
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(CURDIR)/src/main.c $(CURDIR)/src/clocks.c $(CURDIR)/src/platform.c $(CURDIR)/src/settings.c $(CURDIR)/src/stress.c
NXDK_DIR ?= $(CURDIR)/../nxdk
NXDK_SDL = y

include $(NXDK_DIR)/Makefile
