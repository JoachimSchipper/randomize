.PHONY: all clean

# Define HAVE_ARC4RANDOM on platforms that have arc4random for better
# performance. Define HAVE_SIGINFO on platforms that support SIGINFO to enable
# printing data on the console on receipt of SIGINFO.
DEFINES=-DHAVE_ARC4RANDOM -DHAVE_SIGINFO
CFLAGS=-std=c99 -pedantic -W -Wall -Wno-unused-parameter -Wno-sign-compare -Wbad-function-cast -Wcast-align -Wchar-subscripts -Wundef -Wfloat-equal -Wmissing-declarations -Wmissing-noreturn -Wmissing-format-attribute -Wmissing-prototypes -Wnested-externs -Wpointer-arith -Wshadow -Wstrict-prototypes -Wwrite-strings -g -O2 ${DEFINES}

all: randomize

clean:
	rm -f randomize
