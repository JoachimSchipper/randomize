.PHONY: all clean cscope dev test

# Define HAVE_ARC4RANDOM on platforms that have arc4random_uniform() to obtain fast and
# decent random numbers.
#
# Define HAVE_SRANDOMDEV for platforms that have srandomdev() to obtain
# semi-decent random numbers via random(3). (The arc4random_uniform() interface
# is preferred if available.)
#
# Define HAVE_SIGINFO on platforms that support SIGINFO to enable printing data
# on the console on receipt of SIGINFO.
#
# Define HAVE_VIS on platforms that have a vis(3) routine, HAVE_STRLCAT on
# platforms that have strlcat(3), and HAVE_STRTONUM on platforms that have
# strtonum(3); in each case, a replacement is used if the function is not
# available.
#
DEFINES=-DHAVE_ARC4RANDOM -DHAVE_SRANDOMDEV -DHAVE_SIGINFO -DHAVE_VIS \
	-DHAVE_STRLCAT -DHAVE_STRTONUM
# Tell glibc that we want access to more functions.
DEFINES+=-D_BSD_SOURCE -D_GNU_SOURCE
# Warns on pretty much everything, except two conditions (signed compare and
# unused parameters) that are not necessarily errors.
#
# -Wpadded or the OpenBSD extension -Wlarger-than-X may occasionally be useful
# as well.
CFLAGS=-std=c99 -pedantic -W -Wall -Wno-sign-compare -Wno-unused-parameter -Wbad-function-cast -Wcast-align -Wcast-qual -Wchar-subscripts -Wfloat-equal -Wmissing-declarations -Wmissing-format-attribute -Wmissing-noreturn -Wmissing-prototypes -Wnested-externs -Wpointer-arith -Wshadow -Wstrict-prototypes -Wwrite-strings -Wundef -Werror -g -O2 -I/usr/local/include ${DEFINES}
# -Wredundant-decls
LDFLAGS=-L/usr/local/lib
LIBS=-lpcre
HEADERS=compat.h record.h
OBJS=compat.o record.o randomize.o
SRCS=${OBJS:.o=.c} ${HEADERS}
# For systems with groff but no mandoc, use
#MANDOC=groff -mandoc
MANDOC?=mandoc

all: randomize randomize.cat1

clean:
	rm -f randomize randomize.cat1 ${OBJS} test/{1,2,3,4,5}.result tags cscope.out cscope.in.out cscope.po.out

randomize: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o randomize ${OBJS} ${LIBS}

test: randomize test/1.in test/1.out test/2.in test/2.out test/3.in test/3.out test/4a.in test/4b.in test/4c.in test/4.out test/5.in test/5.out
	# Simplest case, but file has no eol
	./randomize test/1.in | env LC_ALL=C sort > test/1.result &&\
		diff -u test/1.out test/1.result
	# Error on no end of line
	./randomize -e '\n' -o '&' test/1.in >/dev/null &&\
		echo 'This is not supposed to work' >&2 &&\
		exit 1 || true
	# Error on zero-width match
	./randomize -e '.*?' -o '&' test/1.in >/dev/null &&\
		echo 'This is not supposed to work' >&2 &&\
		exit 1 || true
	# Randomize arguments
	./randomize -e 'ignored' -o '\n' -an 10 `cat test/1.in` |\
		env LC_ALL=C sort > test/1.result &&\
		diff -u test/1.out test/1.result
	# Reading from pipe (long file, partially in memory)
	cat test/2.in | ./randomize | env LC_ALL=C sort > test/2.result &&\
		diff -u test/2.out test/2.result
	# Long lines
	./randomize test/3.in | env LC_ALL=C sort > test/3.result &&\
		diff -u test/3.out test/3.result
	# Multiple files
	cat test/4a.in | ./randomize - test/4b.in test/4c.in |\
		env LC_ALL=C sort > test/4.result &&\
		diff -u test/4.out test/4.result
	# Regular expression and escape support
	./randomize -e '(.*?)([ \t])' -o '\0\x0\xb\xB\2\1\n' test/5.in |\
		env LC_ALL=C sort > test/5.result &&\
		diff -u test/5.out test/5.result
	# Multiple files, changing -e/-o midway
	cat test/4a.in | (cd test/ && ../randomize -o '&' - -e '(.*?)([ \t])' -o '\1 [5.in]\n' 5.in -e '\n' -o '\n' -- -e) |\
		env LC_ALL=C sort > test/6.result &&\
		diff -u test/6.out test/6.result
	# Requesting a few lines
	./randomize -n 1 test/2.in >/dev/null || exit 1;
	cat test/2.in | ./randomize -n 1 >/dev/null || exit 1;
	./randomize -n 512 test/2.in >/dev/null || exit 1;
	cat test/2.in | ./randomize -n 512 >/dev/null || exit 1;
	./randomize -n 4096 test/2.in | env LC_ALL=C sort > test/2.result &&\
		diff -u test/2.out test/2.result
	cat test/2.in | ./randomize -n 4096 |\
		env LC_ALL=C sort > test/2.result &&\
		diff -u test/2.out test/2.result

${OBJS}: ${HEADERS}

randomize.cat1: randomize.1
	${MANDOC} -Tascii randomize.1 > randomize.cat1

tags: ${SRCS}
	ctags ${SRCS}

cscope: cscope.out

cscope.out cscope.in.out cscope.po.out: ${SRCS}
	cscope -bq ${SRCS}

dev: all tags cscope test
