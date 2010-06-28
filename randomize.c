/*
 * Copyright (c) 2009 Joachim Schipper <joachim@joachimschipper.nl>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Various randomization algorithms.
 */

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifdef HAVE_SIGINFO
#include <signal.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pcre.h>

#include "record.h"

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

/* XXX Handle SIGPIPE better (die silently) */
#ifdef HAVE_SIGINFO
/* XXX Support this properly */
static volatile sig_atomic_t got_siginfo = 0, write_delimited_handled_siginfo = 0;
static void handle_siginfo(int sig);
#endif

int main(int argc, char **argv);

/* XXX const char *usage = "randomize [-o str] [-i str [-i str ...]] [file ...]\nrandomize [-o str] (-a | -e) [arg ...]\n"; */
const char *usage = "randomize [-e regex] [-o str] [-n number] [file [file ...]]\n";

#ifdef HAVE_ARC4RANDOM
#define random_uniform(max) arc4random_uniform(max)
#else
/*
 * Return a random number chosen uniformly from 0, 1, ..., max - 1.
 */
/* XXX Does this work at all? */
uint32_t random_uniform(uint32_t max);
uint32_t
random_uniform(uint32_t max)
{
	uint32_t	 r;

	while ((r = random()) > UINT32_MAX - UINT32_MAX % max);

	return r % max;
}
#endif

#ifdef HAVE_SIGINFO
static void
handle_siginfo(int sig)
{
	assert(sig == SIGINFO);

	got_siginfo = 1;
	write_delimited_handled_siginfo = 0;
}
#endif

int
main(int argc, char **argv)
{
	const char	*re_str, *output_str, *errstr;
	char		 ch;
	int		 fd, error_offset;
	unsigned int	 i, last;
	uint_fast32_t	 r, nrecords, rec_size, j;
	struct rec_file
		       **f;
	void		*tmp;
	struct {
		off_t	 offset;
		struct rec_file
			*f;
		int	 len;
	}		*rec;
	pcre		*re;
	pcre_extra	*re_extra;
#ifndef NDEBUG
	/* XXX Significantly hurts performance */
	char		*p;
#endif
#ifdef HAVE_SIGINFO
	struct sigaction act;
#endif

#ifdef HAVE_SIGINFO
	/*
	 * Enable SIGINFO handler
	 */
	act.sa_handler = handle_siginfo;
	/* XXX We do, right? */
	/* We do *not* wish to restart read() and the like on SIGINFO */
	act.sa_flags = 0;
	/* Do not block any other signals while handling this signal */
	act.sa_mask = 0;

	sigaction(SIGINFO, &act, NULL);
#endif

	re = NULL;
	re_extra = NULL;
	tmp = NULL;

	/* Defaults */
	re_str = "\n";
	output_str = "&";
	nrecords = 0;

	while ((ch = getopt(argc, argv, "e:n:o:")) != -1) {
		switch (ch) {
		case 'e':
			re_str = optarg;
			break;
		case 'n':
			nrecords = strtonum(optarg, 1, MIN(SIZE_MAX, LLONG_MAX), &errstr);
			if (errstr)
				errx(1, "number of records is %s: %s", errstr, optarg);
			break;
		case 'o':
			output_str = optarg;
			break;
		default:
			assert(ch == '?');
			fprintf(stderr, "%s", usage);
			exit(127);
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	/* XXX Document options */
	/* XXX Do we need to call maketables()? */
	if ((re = pcre_compile(re_str, PCRE_DOTALL | PCRE_EXTRA | PCRE_MULTILINE, &errstr, &error_offset, NULL)) == NULL)
		errx(1, "Failed to parse regular expression %s: %s at %d", re_str, errstr, error_offset);
	re_extra = pcre_study(re, 0, &errstr);
	if (errstr != NULL)
		errx(1, "Failed to study regular expression %s: %s", re_str, errstr);

	if ((f = malloc((argc == 0 ? 1 : argc) * sizeof(*f))) == NULL)
		err(1, "Failed to allocate memory for files");

	if (argc == 0) {
		if ((f[0] = rec_open(0, re, re_extra)) == NULL)
			err(1, "Failed to open stdin");
	} else {
		for (i = 0; i < argc; i++) {
			if (strcmp(argv[i], "-") == 0)
				fd = 0;
			else {
				if ((fd = open(argv[i], O_RDONLY, 0644)) == -1)
					err(1, "Failed to open %s", argv[i]);
			}

			if ((f[i] = rec_open(fd, re, re_extra)) == NULL)
				err(1, "Failed to rec_open %s", strcmp(argv[i], "-") == 0 ? "stdin" : argv[i]);
			/* XXX */
			assert(rec_fd(f[i]) != -1);
		}
	}

	/* XXX Support -n */
	assert(nrecords == 0);
	if ((rec = malloc((rec_size = 1) * sizeof(*rec))) == NULL) /* XXX Enlarge */
		err(1, "Failed to allocate memory for records");

	j = last = 0;
	for (i = 0; i < 1 || i < argc; i++) {
		while (1) {
			/* Loop invariant: rec[0] to rec[j] is random */
			if (j >= rec_size) {
				if (rec_size > MIN(UINT32_MAX / 2, SIZE_MAX / 2 / sizeof(*rec)))
					err(1, "Too many records");
				if ((tmp = realloc(rec, 2 * rec_size * sizeof(*rec))) == NULL)
					err(1, "Failed to allocate memory for more records");

				rec = tmp;
				rec_size *= 2;
			}

			rec[j] = rec[r = random_uniform(j + 1)];
			rec[r].f = f[i];
#ifndef NDEBUG
			if ((rec[r].len = rec_next(f[i], &rec[r].offset, &p)) == -1)
#else
			if ((rec[r].len = rec_next(f[i], &rec[r].offset, NULL)) == -1)
#endif
			{
				if (errno == 0) {
					rec[r] = rec[j];
					/* EOF */
					break;
				} else
					err(1, "Failed to read from %s", argc == 0 || strcmp(argv[i], "-") == 0 ? "stdin" : argv[i]);
			}
			assert(rec[r].len > 0);
#ifndef NDEBUG
			if (strcmp(re_str, "\n"))
				assert(p[rec[r].len - 1] == '\n');
			free(p);
#endif
			last = r;

			j++;
		}
	}

	/* Write out data */
	/* XXX Elevator algorithm? */
	for (i = 0; i < j; i++)
		if (rec_write_offset(rec[i].f, rec[i].offset, rec[i].len, i == last, output_str, stdout) != 0)
			err(1, "Failed to write output");

	for (i = 0; i < argc; i++) {
		fd = rec_fd(f[i]);
		if (rec_close(f[i]) != 0)
			err(1, "Failed to rec_close %s", argc == 0 ? "(stdin)" : argv[i]);
		if (close(fd) != 0) /* XXX Loop on EINTR */
			err(1, "Failed to close %s", argc == 0 ? "(stdin)" : argv[i]);
	}

	exit(0);
}
