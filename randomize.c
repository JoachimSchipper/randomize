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
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
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
	int		 fd, error_offset, rv;
	unsigned int	 f_no, i;
	uint_fast32_t	 r, nrecords, rec_size, rec_no, *last;
	struct rec_file
		       **f;
	void		*tmp;
	struct {
		off_t	 offset;
		int	 len, f_no;
	}		*rec;
	pcre		*re;
	pcre_extra	*re_extra;
#ifdef HAVE_SIGINFO
	struct sigaction act;
#endif

#ifdef HAVE_SIGINFO
	/*
	 * Enable SIGINFO handler
	 */
	act.sa_handler = handle_siginfo;
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
	nrecords = UINT32_MAX;

	while ((ch = getopt(argc, argv, "e:n:o:")) != -1) {
		switch (ch) {
		case 'e':
			re_str = optarg;
			break;
		case 'n':
			nrecords = strtonum(optarg, 1, UINT32_MAX - 1, &errstr);
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

	if (argc > SIZE_MAX / MAX(sizeof(*f), sizeof(*last))) {
		errno = ENOMEM;
		err(1, "Failed to allocate memory for files and end-of-file markers");
	}
	if ((f = malloc((argc == 0 ? 1 : argc) * sizeof(*f))) == NULL)
		err(1, "Failed to allocate memory for files");
	if ((last = malloc((argc == 0 ? 1 : argc) * sizeof(*last))) == NULL)
		err(1, "Failed to allocate memory for end-of-file markers");

	if ((rec = malloc((rec_size = 128) * sizeof(*rec))) == NULL)
		err(1, "Failed to allocate memory for records");

	rec_no = 0;
	for (f_no = 0; f_no < 1 || f_no < argc; f_no++) {
		/* Open file */
		if (argc == 0 || strcmp(argv[f_no], "-") == 0)
			fd = 0;
		else
			if ((fd = open(argv[f_no], O_RDONLY, 0644)) == -1)
				err(1, "Failed to open %s", argv[f_no]);

		if ((f[f_no] = rec_open(fd, re, re_extra)) == NULL)
			err(1, "Failed to rec_open %s", strcmp(argv[f_no], "-") == 0 ? "stdin" : argv[f_no]);

		while (1) {
			/*
			 * Loop invariant: rec_no is the number of the current
			 * record (among all files), and rec[0] through
			 * rec[MIN(rec_no, nrecords)] is a random selection of
			 * distinct records from among all records we have
			 * already seen.
			 */
			if (MIN(rec_no, nrecords) >= rec_size) {
				if (rec_size > MIN(UINT32_MAX / 2, SIZE_MAX / 2 / sizeof(*rec)))
					err(1, "Too many records");
				if ((tmp = realloc(rec, 2 * rec_size * sizeof(*rec))) == NULL)
					err(1, "Failed to allocate memory for more records");

				rec = tmp;
				rec_size *= 2;
			}

			r = random_uniform(rec_no + 1);
			if (r > nrecords)
				r = nrecords;
			rec[MIN(rec_no, nrecords)] = rec[r];

			rec[r].f_no = f_no;
			while ((rec[r].len = rec_next(f[f_no], &rec[r].offset, NULL)) == -1 && (errno == EINTR || errno == EAGAIN));
			if (rec[r].len == -1) {
				if (errno == 0) {
					/* EOF */
					rec[r] = rec[MIN(rec_no, nrecords)];
					assert(r == MIN(rec_no, nrecords) || rec[r].len > 0);
					break;
				} else
					errx(1, "Failed to read from %s: %s%s",
					    argc == 0 || strcmp(argv[f_no], "-") == 0 ? "stdin" : argv[f_no],
					    strerror(errno),
					    errno == EINVAL ? " or error in regular expression" : "");
			}

			assert(rec[r].len > 0);
			last[f_no] = r;
			if (++rec_no == UINT32_MAX - 1)
				errx(1, "Too many records");
		}
	}

	/* Write out data */
	/* XXX Elevator algorithm? */
	for (i = 0; i < rec_no && (nrecords == 0 || i < nrecords); i++)
		if ((errstr = rec_write_offset(f[rec[i].f_no], rec[i].offset, rec[i].len, i == last[rec[i].f_no], output_str, stdout)) != NULL)
			errx(1, "%s", errstr);

	for (i = 0; i < argc; i++) {
		fd = rec_fd(f[i]);
		while ((rv = rec_close(f[i])) != 0 && (errno == EINTR || errno == EAGAIN));
		if (rv != 0)
			err(1, "Failed to rec_close %s", argc == 0 ? "stdin" : argv[i]);
		while ((rv = close(fd)) != 0 && (errno == EINTR || errno == EAGAIN));
		if (rv != 0)
			err(1, "Failed to close %s", argc == 0 ? "stdin" : argv[i]);
	}

	exit(0);
}
