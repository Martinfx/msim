#
# Copyright (c) 2008 Martin Decky
# All rights reserved.
#
# Distributed under the terms of GPL.
#

CC = cc
CFLAGS =  -Wall -g -O3 -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes -I/usr/local/include -L/usr/local/lib
LIBS = -lreadline
CP = cp
MV = mv
RM = rm
DIFF = diff
MAKEDEPEND = makedepend

DEPEND = Makefile.depend
DEPEND_PREV = $(DEPEND).prev
TARGET = ./../bin/msim

SOURCES = \
	utils.c \
	fault.c \
	text.c \
	env.c \
	cline.c \
	cmd.c \
	check.c \
	main.c \
	parser.c \
	list.c \
	io/input.c \
	io/output.c \
	cpu/cpu.c \
	cpu/instr.c \
	debug/debug.c \
	debug/gdb.c \
	debug/breakpoint.c \
	device/machine.c \
	device/mem.c \
	device/ddisk.c \
	device/dcpu.c \
	device/dkeyboard.c \
	device/dorder.c \
	device/dprinter.c \
	device/dtime.c \
	device/device.c \
	arch/win32/mmap.c \
	arch/win32/stdin.c \
	arch/win32/signal.c \
	arch/posix/stdin.c \
	arch/posix/signal.c

OBJECTS := $(addsuffix .o,$(basename $(SOURCES)))

.PHONY: all clean distclean

all: $(TARGET)
	-[ -f $(DEPEND) ] && $(CP) -a $(DEPEND) $(DEPEND_PREV)

clean:
	$(RM) -f $(TARGET) $(OBJECTS) $(DEPEND) $(DEPEND_PREV)

distclean: clean
	$(RM) -f Makefile

-include $(DEPEND)

$(TARGET): $(OBJECTS) $(DEPEND)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LIBS)

%.o: %.c $(DEPEND)
	$(CC) -c $(CFLAGS) -o $@ $<

$(DEPEND):
	$(MAKEDEPEND) -f - -- $(CFLAGS) -- $(SOURCES) > $@ 2> /dev/null
	-[ -f $(DEPEND_PREV) ] && $(DIFF) -q $(DEPEND_PREV) $@ && $(MV) -f $(DEPEND_PREV) $@
