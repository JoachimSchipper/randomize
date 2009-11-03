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

#include <sys/types.h>
#include <sys/uio.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#ifdef HAVE_SIGINFO
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef __GNUC__
#define attribute(x) /* Not supported on non-GCC compilers */
#endif

/* Used for input and output delimiters */
struct delim {
	const char	*chars;
	unsigned int	 size;
};

#ifdef HAVE_SIGINFO
static sig_atomic_t got_siginfo = 0, write_delimited_handled_siginfo = 0;
static void handle_siginfo(int sig);
#endif
/* 
 * Rewrite string to a version with all C escape sequences unescaped (e.g. the
 * two characters '\' and 'n' become a single '\n'). Handles octal ('\0'),
 * hexadecimal ('\0x0f'), and all special sequences ('\n', '\r', etc.).
 *
 * Returns the size of the resulting data (which is not NUL-terminated and may
 * contain NULs).
 */
size_t unescape(char *str) __attribute__((nonnull(1)));
/*
 * Write nbytes bytes from buf, and the contents of output_delimiter, to file
 * descriptor d. If HAVE_SIGINFO and a SIGINFO signal is received, print
 * progress in the current buffer but do not clear got_siginfo. Returns -1 on
 * error, or the number of bytes written, or SSIZE_MAX if this number is larger
 * than SSIZE_MAX.
 *
 * On error, sets errno to any of the values defined for realloc(3) or
 * writev(2).
 */
ssize_t write_delimited(int d, void *buf, size_t nbytes, const struct delim output_delimiter) __attribute__((nonnull(2)));
/* Compare struct delims for size, reversed (i.e. larger delims are "smaller") */
static inline int delimiter_cmp_size(const void *d1, const void *d2) __attribute__((nonnull(1, 2), pure));
int main(int argc, char **argv);

const char *usage = "randomize [-o str] [-i str [-i str ...]] [file ...]\nrandomize [-o str] (-a | -e) [arg ...]\n";

#ifndef HAVE_ARC4RANDOM
/*
 * Return a random number chosen uniformly from 0, 1, ..., max - 1.
 */
uint32_t random_uniform(uint32_t max);
#define RANDOM(max) random_uniform(max)
uint32_t
random_uniform(uint32_t max)
{
	uint32_t	 r;

	r = random();

	if (r > (UINT32_MAX - (UINT32_MAX % max)))
		/* Try again to make sure we really are random */
		return random_uniform(max);

	return r % max;
}
#else
#define RANDOM(max) arc4random_uniform(max)
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

size_t
unescape(char *str)
{
	char		*p, *pw, ch;
	const struct {
		char	 ch, stands_for;
	} escape_sequences[] = {
		{ '0',	'\0' },
		{ 'a',	'\a' },
		{ 'b',	'\b' },
		{ 'f',	'\f' },
		{ 'n',	'\n' },
		{ 'r',	'\r' },
		{ 't',	'\t' },
		{ 'v',	'\v' },
		{ '\\',	'\\' },
		{ '?', 	'\?' }
	};
	int		 i;
	unsigned int	 skip;
	size_t		 len;

	for (p = pw = str, len = 0; *p != '\0'; len++) {
		/*
		 * Parse input, looking ahead if we see a backslash. We produce
		 * one output character per loop through the cycle.
		 */
		if (*p != '\\' || p[1] == '\0') {
			*pw++ = *p++;
		} else if ((p[1] >= '0' && p[1] <= '7') || (p[1] == 'x' && isxdigit(p[2]))) {
			/* Octal or hex */

			/* Set skip to the first character not included in the number */
			if (p[1] != 'x')
				for (skip = 2; skip < 4 && p[skip] >= '0' && p[skip] <= '7'; skip++);
			else
				for (skip = 3; skip < 4 && isxdigit(p[skip]); skip++);

			ch = p[skip];
			p[skip] = '\0';

			/* LINTED value is in 0..255, so fits in *pw (optionally after converting to signed) */
			*pw++ = strtoul(p + (p[1] != 'x' ? 1 : 2), NULL, p[1] != 'x' ? 8 : 16);

			*(p += skip) = ch;
		} else {
			/* LINTED i is nonnegative, so can be compared against size_t */
			for (i = 0; i < sizeof(escape_sequences) / sizeof(*escape_sequences); i++) {
				/* \n or something similar */
				if (p[1] == escape_sequences[i].ch) {
					*pw++ = escape_sequences[i].stands_for;
					p += 2;

					break;
				}
			}

			;; /* LINTED as above */
			if (i == sizeof(escape_sequences) / sizeof(*escape_sequences)) {
				/* Some unidentified escape sequence, e.g. \l. */
				*pw++ = p[1];
				p += 2;
			}
		}
	}

	return len;
}

ssize_t
write_delimited(int d, void *buf, size_t nbytes, const struct delim output_delimiter)
{
	struct iovec	 iov[2];
	static void	*out_chars = NULL;
	void		*tmp;
	static unsigned int
			 out_chars_size = 0;
	ssize_t		 bytes_written, to_write;
#ifdef HAVE_SIGINFO
	size_t		 total_bytes_written;

	total_bytes_written = 0;
#endif

	/* Ensure we have a copy of output_delimiter.chars */
	if (out_chars_size < output_delimiter.size) {
		if ((tmp = realloc(out_chars, output_delimiter.size)) == NULL)
			return -1;

		out_chars = tmp;
		out_chars_size = output_delimiter.size;
	}
	memcpy(out_chars, output_delimiter.chars, output_delimiter.size);

	/* Set up iovec */
	iov[0].iov_base = buf;
	iov[0].iov_len = nbytes;
	iov[1].iov_base = out_chars;
	iov[1].iov_len = output_delimiter.size;
	to_write = iov[0].iov_len > SSIZE_MAX - iov[1].iov_len ? SSIZE_MAX : iov[0].iov_len + iov[1].iov_len;

	/* Write until done */
	while ((bytes_written = writev(d, iov, sizeof(iov) / sizeof(*iov))) != 0) {
#ifdef HAVE_SIGINFO
		if (got_siginfo && !write_delimited_handled_siginfo) {
			fprintf(stderr, "Written %zd/%zd bytes... ", total_bytes_written, to_write);
			write_delimited_handled_siginfo = 1;
		}
#endif

		if (bytes_written == -1) {
			if (errno == EINTR)
				continue;
			else
				return -1;
		}

		assert(bytes_written >= 0);

		/* LINTED bytes_written is nonnegative, so can be converted to unsigned */
		if (bytes_written == iov[0].iov_len + iov[1].iov_len)
			break;
		/* LINTED idem */
		else if (bytes_written > iov[0].iov_len) {
			iov[0].iov_len = 0;
			/* LINTED idem */
			bytes_written -= iov[0].iov_len;
			iov[1].iov_base = ((char *) iov[1].iov_base) + bytes_written;
			/* LINTED idem */
			iov[1].iov_len -= bytes_written;
		} else {
			iov[0].iov_base = ((char *) iov[0].iov_base) + bytes_written;
			/* LINTED idem */
			iov[0].iov_len -= bytes_written;
		}

#ifdef HAVE_SIGINFO
		/* LINTED idem */
		total_bytes_written = total_bytes_written > SIZE_MAX - bytes_written ? SIZE_MAX : total_bytes_written + bytes_written;
#endif
	}

	return to_write;
}

int
main(int argc, char **argv)
{
	char		*buf, newline[] = "\n";
	struct delim 	*input_delimiter, output_delimiter;
	enum {
		MODE_READ,
		MODE_RANDOMIZE,
		MODE_UNESCAPE_RANDOMIZE
	}		 mode;
	int		 ch, fd;
	uint32_t	 nlines, new_nlines, i;
	ssize_t		 bytes_read;
	struct {
		size_t	 start, len;
	}		*line;
	size_t		 buf_size, j, oldj, input_delimiters_size, k, r;
	void		*tmp;
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

	/* "Unitialized" */
	input_delimiter = NULL;
	input_delimiters_size = 0;
	output_delimiter.chars = newline;
	output_delimiter.size = 1;

	/* XXX Abstract input/output */
	/* XXX Add -n option */
	mode = MODE_READ;
	while ((ch = getopt(argc, argv, "aei:o:")) != -1) {
		switch (ch) {
		case 'a':
			if (mode != MODE_READ) {
				fprintf(stderr, "%s", usage);
				exit(127);
			}

			mode = MODE_RANDOMIZE;
			break;
		case 'e':
			if (mode != MODE_READ) {
				fprintf(stderr, "%s", usage);
				exit(127);
			}

			mode = MODE_UNESCAPE_RANDOMIZE;
			break;
		case 'i':
			if (input_delimiters_size + 1 > SIZE_MAX / sizeof(*input_delimiter))
				errx(1, "Too many input delimiters - something went horribly wrong!");
			if ((tmp = realloc(input_delimiter, ++input_delimiters_size * sizeof(*input_delimiter))) == NULL)
				err(1, "Failed to allocate more input delimiters");
			input_delimiter = tmp;
			buf = strdup(optarg);
			assert(input_delimiters_size < SSIZE_MAX);
#if ARG_MAX > UINT_MAX
#error ARG_MAX too large
#endif
			/* LINTED input_delimiters_size can be converted to signed, as sizeof(*input_delimiter) >= 2; we can truncate the return value of unescape() at UINT_MAX, as ARG_MAX is smaller */
			input_delimiter[input_delimiters_size - 1].size = unescape(buf);
			/* LINTED idem */
			input_delimiter[input_delimiters_size - 1].chars = buf;

			/* LINTED idem */
			if (input_delimiter[input_delimiters_size - 1].size == 0)
				errx(127, "Zero-length input delimiters are not meaningful");

			mode = MODE_READ;
			break;
		case 'o':
			buf = strdup(optarg);
			/* LINTED as above */
			output_delimiter.size = unescape(buf);
			output_delimiter.chars = buf;
			break;
		default:
			fprintf(stderr, "%s", usage);
			exit(127);
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (mode == MODE_RANDOMIZE || mode == MODE_UNESCAPE_RANDOMIZE) {
		/*
		 * Randomize arguments instead of files.
		 *
		 * This is a *lot* simpler, since the records are already in
		 * memory and easily accessible via argv[].
		 */
		/* LINTED argc is nonnegative, so conversion works */
		for (j = 0; j < argc; j++) {
			/* LINTED as above */
			r = RANDOM(argc - j <= UINT32_MAX ? argc - j : UINT32_MAX) + j;

			if (mode == MODE_UNESCAPE_RANDOMIZE) {
				/*
				 * Just print string
				 */
				/* LINTED r can be converted to signed */
				assert(r < argc);
				/* LINTED idem */
				k = strlen(argv[r]);
			} else {
				/*
				 * Evaluate escape sequences
				 */
				/* LINTED idem */
				assert(r < argc);
				/* LINTED idem */
				k = unescape(argv[r]);
			}

			/* LINTED as above */
			if (write_delimited(1, argv[r], k, output_delimiter) == -1)
				err(1, "Error while writing to stdout");

#ifdef HAVE_SIGINFO
			if (got_siginfo) {
				got_siginfo = 0;
				fprintf(stderr, "Written %zu/%d arguments\n", j, argc);
			}
#endif

			;; /* LINTED as above */
			argv[r] = argv[j];
		}

		exit(0);
	}

	/*
	 * Randomize the contents of the files named on the command line
	 */
	assert(mode == MODE_READ);

	/* Use defaults if not initalized */
	if (input_delimiter == NULL) {
		if ((input_delimiter = malloc((input_delimiters_size = 1) * sizeof(*input_delimiter))) == NULL)
			err(1, "Failed to allocate default input delimiters");
		input_delimiter[0].chars = newline;
		input_delimiter[0].size = 1;
	}

	if (argv[0] == NULL) {
		if ((argv = malloc(2 * sizeof(*argv))) == NULL)
			err(1, "Failed to allocate synthetic argument vector");

		/* strdup() is not necessary, but does keep gcc happy */
		argv[0] = strdup("-");
		argv[1] = NULL;
	}

	/* Sort by length (longest first) */
	qsort(input_delimiter, input_delimiters_size, sizeof(*input_delimiter), delimiter_cmp_size);

	/*
	 * Read *all* data into buf. Find lines, add them to line[], and
	 * newline-terminate them.
	 */
	if ((buf = malloc(buf_size = BUFSIZ)) == NULL)
		err(1, "Failed to allocate memory for buffer\n");
	if ((line = malloc((nlines = 16) * sizeof(*line))) == NULL)
		err(1, "Failed to allocate memory for lines\n");

	oldj = j = 0;
	line[i = 0].start = 0;
	for ( ; *argv != NULL; argv++) {
		if (strcmp(*argv, "-") == 0) {
			/* stdin */
			fd = 0;
		} else {
			if ((fd = open(*argv, O_RDONLY, 0000)) == -1)
				err(1, "Failed to read file %s", *argv);
		}

		/* Slurp the file into memory and parse records */
		/* LINTED j is an index into buf, so this works */
		while ((bytes_read = read(fd, buf + j, buf_size - j - 1)) != 0) {
#ifdef HAVE_SIGINFO
			if (got_siginfo) {
				fprintf(stderr, "Read %zu bytes containing %" PRIu32 " lines\n", j, i);
				got_siginfo = 0;
			}
#endif

			if (bytes_read == -1) {
				if (errno == EINTR)
					/* Try again */
					continue;
				else
					err(1, "Error while reading from %s", (strcmp(*argv, "-") == 0 ? "stdin" : *argv));
			}

			assert(bytes_read >= 0);

			/* LINTED bytes_read is nonnegative, j is an index into buf */
			j += bytes_read;

			if (buf_size - j < BUFSIZ) {
				if ((tmp = realloc(buf, buf_size <= SIZE_MAX / 2 ? 2 * buf_size : SIZE_MAX)) == NULL)
					err(1, "Failed to enlarge buffer");

				buf = tmp;
				buf_size = buf_size <= SIZE_MAX / 2 ? 2 * buf_size : SIZE_MAX;
			}

			/* Parse records */
			for ( ; oldj < j; oldj++) {
				for (k = 0; k < input_delimiters_size; k++) {
					/* LINTED converting to signed works */
					if (j - oldj >= input_delimiter[k].size && memcmp(&buf[oldj], input_delimiter[k].chars, input_delimiter[k].size) == 0) {
						if (i == nlines - 1) {
							if (nlines > MIN(UINT32_MAX / 2, SIZE_MAX / 2 / sizeof(*line)))
								new_nlines = MIN(UINT32_MAX, SIZE_MAX / sizeof(*line));
							else
								new_nlines = 2 * nlines;
							if ((tmp = realloc(line, new_nlines * sizeof(*line))) == NULL)
								errx(1, "Failed to enlarge line buffer");

							line = tmp;
							nlines *= new_nlines;
						}

						line[i].len = oldj - line[i].start;

						/* May point 1 past bufp, but we'll fix that below */
						/* LINTED converting k to signed works */
						oldj += input_delimiter[k].size - 1;
						line[++i].start = oldj + 1;

						break;
					}
				}
			}
		}

		while (close(fd) != 0 && errno != EINTR);
	}

	/* Set nlines to actual number of lines used */
	line[i].len = j - line[i].start;
	if (line[i].start >= j)
		nlines = i;
	else
		nlines = i + 1;

	/* Print in random order */
	for (i = 0; i < nlines; i++) {
		r = RANDOM(nlines - i) + i;

		/* LINTED converting r to signed works, since r is size_t */
		if (write_delimited(1, &buf[line[r].start], line[r].len, output_delimiter) == -1)
			err(1, "Error while writing to stdout");

#ifdef HAVE_SIGINFO
		if (got_siginfo) {
			got_siginfo = 0;
			fprintf(stderr, "Written %" PRIu32 "/%" PRIu32 " lines\n", i, nlines);
		}
#endif

		;; /* LINTED as above */
		line[r] = line[i];
	}

	exit(0);
}

static inline int
delimiter_cmp_size(const void *d1, const void *d2)
{
	const struct delim	*dl1 = d1, *dl2 = d2;

	if (dl1->size > dl2->size)
		return -1;
	if (dl1->size < dl2->size)
		return 1;
	return 0;
}
