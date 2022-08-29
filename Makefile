#
#use cc compiler on Solaris and gcc on Linux.
#use gmake on Solaris and make in Linux
#
#the library is put in the directy where it is compiled,
#and the test binary should also be compiled and run there
#
OS = $(shell uname -s)
DIR = $(shell pwd)
ifeq ($(OS), SunOS)
CC = cc
else
CC = gcc
endif

LIBNAME = mymem
LIBNAMESO = lib$(LIBNAME).so
TESTNAME = mytest
LIBCFILE = code.c
MAINCFILE = test.c
FLAGS = -m64 -g

ifeq ($(CC),gcc)
LIBFLAGS = $(FLAGS) -shared -fPIC
MAINFLAGS = $(FLAGS) -pthread -L$(DIR) -Wl,-rpath,$(DIR)
else
LIBFLAGS = $(FLAGS) -G -Kpic
MAINFLAGS = $(FLAGS) -mt -L$(DIR) -R$(DIR)
endif

$(TESTNAME): $(MAINCFILE) $(LIBNAME)
	$(CC) $(MAINCFILE) $(MAINFLAGS) -l$(LIBNAME) -o $(TESTNAME)
$(LIBNAME): $(LIBCFILE)
	$(CC) $(LIBCFILE) $(LIBFLAGS) -o $(LIBNAMESO)
clean:
	rm $(LIBNAMESO) $(TESTNAME)

