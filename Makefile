.PHONY: all clean
DEFINES=-DHAVE_ARC4RANDOM -DHAVE_SIGINFO
CFLAGS=-std=c99 -pedantic -W -Wall -Wno-unused-parameter -Wno-sign-compare -Wbad-function-cast -Wcast-align -Wchar-subscripts -Wundef -Wfloat-equal -Wmissing-declarations -Wmissing-noreturn -Wmissing-format-attribute -Wmissing-prototypes -Wnested-externs -Wpointer-arith -Wshadow -Wstrict-prototypes -Wwrite-strings -g -O2 ${DEFINES}

all: randomize

clean:
	rm -f randomize
