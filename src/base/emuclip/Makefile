top_builddir=../../..
include $(top_builddir)/Makefile.conf

#The C files, include files and dependancies here.
CFILES = emuclip.c

DEPENDS=$(CFILES:.c=.d)
HFILES =

# Insert all source- and header-files here.
ALL = $(CFILES) $(HFILES)

# All object-files are included here.
OBJS = $(CFILES:.c=.o)

all: lib

install:

include $(REALTOPDIR)/src/Makefile.common
