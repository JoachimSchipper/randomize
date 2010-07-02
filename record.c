/*
 * Copyright (c) 2010 Joachim Schipper <joachim@joachimschipper.nl>
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

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pcre.h>

#include "record.h"

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif

/* XXX Fix signal-safety */

struct rec_file {
	char		*buf_p;
	pcre		*re;
	pcre_extra	*re_extra;
	/* Limited to int instead of size_t by pcre_exec() */
	int		 buf_size, buf_first, buf_last;
	off_t		 offset;
	/*
	 * fd is the file descriptor passed to rec_open() and tmp is either
	 * equal to fd (if fd is seekable) or a file descriptor pointing to a
	 * temporary file.
	 */
	int		 fd, tmp;
};

/*
 * Used by pcre_exec(); rec_open() assures it's long enough, and rec_close()
 * deallocates it when all rec_file have been closed.
 */
static int    *ovector = NULL, ovector_size = 0;
static size_t  ovector_refcount = 0;

struct rec_file *
rec_open(int fd, pcre *re, pcre_extra *re_extra)
{
	sigset_t	 set, oset;
	struct stat	 sb;
	struct rec_file	*f;
	char		*template;
	const char	*prefix;
	void		*tmp;
	int		 capturecount, prefix_len;
#ifndef NDEBUG
	int		 rv;
#endif

	template = NULL;
	f = NULL;
	if ((f = malloc(sizeof(*f))) == NULL ||
	    (f->buf_p = malloc(f->buf_size = 4096)) == NULL)
		goto err;

	f->fd = f->tmp = fd;
	f->buf_last = f->buf_first = 0;
	f->offset = 0;
	if (pcre_fullinfo(f->re = re, f->re_extra = re_extra, PCRE_INFO_CAPTURECOUNT, &capturecount) != 0)
		goto err;

	/* Make sure that ovector is usable */
	if (capturecount > INT_MAX / sizeof(*ovector) / 3 - 1) {
		errno = ENOMEM;
		goto err;
	}
	if ((capturecount + 1) * 3 > ovector_size) {
		if ((tmp = realloc(ovector, (capturecount + 1) * 3 * sizeof(*ovector))) == NULL)
			goto err;

		ovector = tmp;
		ovector_size = (capturecount + 1) * 3;
	}
	ovector_refcount++;

	/* If the file is not seek()able, open a temporary file */
#ifndef NDEBUG
	rv =
#endif
		fstat(f->fd, &sb);
	assert(rv == 0);
	if (!S_ISREG(sb.st_mode) && !S_ISCHR(sb.st_mode) && !S_ISBLK(sb.st_mode)) {
		/* XXX Document the use of TMPDIR */
		if ((prefix = getenv("TMPDIR")) == NULL || prefix[0] == '\0')
			prefix = "/tmp";
		for (prefix_len = MIN(strlen(prefix), INT_MAX); prefix_len > 0 && prefix[prefix_len - 1] == '/'; prefix_len--);
		if (asprintf(&template, "%.*s/randomize.XXXXXX", prefix_len, prefix) == -1)
			goto err;

		/*
		 * Create temporary file; block signals to make it more likely
		 * that unlink() succeeds.
		 */
		sigfillset(&set);
		sigprocmask(SIG_BLOCK, &set, &oset);
		f->tmp = mkstemp(template);
		if (f->tmp != -1)
			unlink(template);
		sigprocmask(SIG_SETMASK, &oset, NULL);

		if (f->tmp == -1)
			goto err;
	}

	free(template);

	return f;

err:
	if (f)
		rec_close(f);
	free(template);

	return NULL;
}

int
rec_fd(const struct rec_file *f)
{
	return f->fd;
}

int
rec_close(struct rec_file *f)
{
	int		 rv;

	rv = 0;

	if (f->tmp != -1 && f->tmp != f->fd)
		while ((rv = close(f->tmp)) != 0 && (errno == EINTR || errno == EAGAIN));

	free(f->buf_p);
	free(f);

	if (ovector_refcount == 0) {
		assert(ovector == NULL);
		assert(ovector_size == 0);
	} else {
		ovector_refcount--;
	       	if (ovector_refcount == 0) {
			free(ovector);
			ovector = NULL;
			ovector_size = 0;
		}
	}

	return rv;
}

int
rec_next(struct rec_file *f, off_t *offset, char **p)
{
	void		*tmp;
	ssize_t		 nbytes;
	int		 rv, eof, i, len;
#ifndef NDEBUG
	int		 capturecount;

	assert(pcre_fullinfo(f->re, f->re_extra, PCRE_INFO_CAPTURECOUNT, &capturecount) == 0);
	assert(capturecount <= SIZE_MAX / sizeof(*ovector) / 3 - 1);
	assert((capturecount + 1) * 3 <= ovector_size);
#endif

	/*
	 * Look for regular expression.
	 *
	 * We use the buffer at f->buf_p for this. At any moment,
	 * - f->buf_p[f->buf_first..f->buf_last] is valid, unprocessed data
	 *   (the subject of pcre_exec());
	 * - f->buf_p[0] to f->buf_p[f->buf_first] is valid data that has been
	 *   processed and should be flushed to disk (if using a temporary
	 *   file);
	 * - f->buf_p[f->buf_last] to f->buf_p[f->buf_size] is garbage.
	 *
	 * We run pcre_exec(); if it fails, we read more data, flushing data
	 * and/or enlarging the buffer as necessary.
	 */
	eof = 0;
	while ((rv = pcre_exec(f->re, f->re_extra, f->buf_p, f->buf_last - f->buf_first, f->buf_first, eof ? 0 : PCRE_NOTEOL, ovector, ovector_size)) < 0) {
		if (rv != PCRE_ERROR_NOMATCH) {
			errno = EINVAL;
			goto err;
		}

		if (eof) {
			if (f->buf_first < f->buf_last) {
				/* Unterminated final record */
				ovector[0] = f->buf_last;
				ovector[1] = f->buf_last;
				break;
			}

			/*
			 * All data processed - everything should have been
			 * flushed to disk. Notify the caller of EOF.
			 */
			assert(f->buf_first == 0);
			assert(f->buf_last == 0);

			errno = 0;
			goto err;
		}

		/*
		 * Get more data
		 */

		assert(f->buf_last >= f->buf_first);
		assert(f->buf_size >= f->buf_last);

		if (f->tmp != f->fd) {
			/* Flush processed data to disk */
			nbytes = 0;
			for (i = 0; i < f->buf_first; nbytes = write(f->tmp, &f->buf_p[i], f->buf_first - i)) {
				if (nbytes == -1) {
					if (errno == EINTR || errno == EAGAIN)
						continue;
					else
						goto err;
				}

				assert(nbytes >= 0);
				i += nbytes;
			}
			assert(i == f->buf_first);
		}

		if (f->buf_first > f->buf_size / 2) {
			/* Just move unprocessed data to front */
			bcopy(&f->buf_p[f->buf_first], f->buf_p, f->buf_last - f->buf_first);
		} else {
			/* Enlarge buffer */
			if (f->buf_size > INT_MAX / 2) {
				errno = ENOMEM;
				goto err;
			}

			if ((tmp = malloc(f->buf_size * 2)) == NULL)
				goto err;

			memcpy(tmp, &f->buf_p[f->buf_first], f->buf_last - f->buf_first);

			free(f->buf_p);
			f->buf_p = tmp;
			f->buf_size *= 2;
		}
		f->buf_last -= f->buf_first;
		f->buf_first = 0;
		assert(f->buf_size > f->buf_last);

		/* Read additional data */
		while ((nbytes = read(f->fd, &f->buf_p[f->buf_last], f->buf_size - f->buf_last)) == -1 && (errno == EINTR || errno == EAGAIN));
		if (nbytes == -1)
			goto err;
		if (nbytes == 0)
			eof = 1;
		f->buf_last += nbytes;
	}

	assert(ovector[0] >= f->buf_first);
	assert(ovector[1] >= ovector[0]);
	assert(f->buf_last >= ovector[1]);

	len = ovector[1] - f->buf_first;

	if (offset != NULL)
		*offset = f->offset;

	if (p != NULL) {
		if ((*p = malloc(len)) == NULL)
			goto err;

		memcpy(*p, &f->buf_p[f->buf_first], len);
	}

#ifdef DEBUG_PRINT
	fprintf(stderr, "record: %.*s", len, &f->buf_p[f->buf_first]);
#endif

	f->buf_first = ovector[1];
	f->offset += len;

	return len;

err:
	return -1;
}

const char *
rec_write_offset(struct rec_file *f, off_t offset, int len, int last, const char *delim, FILE *file)
{
	int		 i;
	ssize_t		 nbytes;

	/*
	 * Read data into memory and call rec_write_mem()
	 */

	/* rec_next() could handle this record, after all... */
	assert(len <= f->buf_size);

	nbytes = 0;
	/* XXX Document that this trashes f->buf_p */
	assert(f->buf_first == 0);
	assert(f->buf_first == f->buf_last);
	for (i = 0; i < len; nbytes = pread(f->tmp, &f->buf_p[i], len - i, offset + i)) {
		if (nbytes == -1) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			else {
				snprintf(f->buf_p, f->buf_size, "Failed to read record from file: %s", strerror(errno));
				goto err;
			}
		}

		assert(nbytes >= 0);
		i += nbytes;
	}
	assert(i == len);

#ifdef DEBUG_PRINT
	fprintf(stderr, "printing: %.*s", len, f->buf_p);
#endif

	return rec_write_mem(f, f->buf_p, len, last, delim, file);

err:
	return f->buf_p;
}

const char *
rec_write_mem(struct rec_file *f, const char *p, int len, int last, const char *delim, FILE *file)
{
	int		 rv, i, value;
	enum {
		NORMAL,
		SEEN_BACKSLASH,
		SEEN_OCTAL1,
		SEEN_OCTAL2,
		SEEN_HEX0,
		SEEN_HEX1
	}		 state;
	ssize_t		 nbytes;
#ifndef NDEBUG
	int		 capturecount;

	assert(pcre_fullinfo(f->re, f->re_extra, PCRE_INFO_CAPTURECOUNT, &capturecount) == 0);
	assert(capturecount <= SIZE_MAX / sizeof(*ovector) / 3 - 1);
	assert((capturecount + 1) * 3 <= ovector_size);
#endif

	assert(len > 0);

	if ((rv = pcre_exec(f->re, f->re_extra, p, len, 0, last ? 0 : PCRE_NOTEOL, ovector, ovector_size)) < 0) {
		/* Unterminated final record */
		assert(rv == PCRE_ERROR_NOMATCH);
		assert(last);

		rv = 0;
		ovector[0] = ovector[1] = len;
	} else {
		assert(rv >= 1);
		assert(ovector[1] == len);
	}

	/* Output anything prior to match */
	nbytes = fwrite(p, 1, ovector[0], file);
	if (nbytes != ovector[0]) {
		snprintf(f->buf_p, f->buf_size, "Failed to write output: %s", strerror(errno));
		goto err;
	}

	/* Output delim, handling backreferences and the like */
	state = NORMAL;
	value = 0;
	for (i = 0; i < strlen(delim); i++) {
		switch (state) {
		case NORMAL:
			value = 0;
			switch (delim[i]) {
			case '\\':
				state = SEEN_BACKSLASH;
				break;
			case '&':
				/* Output match; note the label! */
output_match:
				if (value >= rv) {
					if (rv == 0) {
						if (value == 0)
							snprintf(f->buf_p, f->buf_size, "The argument to -o contains & (by default), but the last record is not terminated");
						else
							snprintf(f->buf_p, f->buf_size, "The argument to -o contains \\%d, but the last record is not terminated", value);

						goto err;
					} else {
						assert(value > 0);
						snprintf(f->buf_p, f->buf_size, "Invalid backreference \\%d", value);
						goto err;
					}
				}
				nbytes = fwrite(&p[ovector[2 * value]], 1, ovector[2 * value + 1] - ovector[2 * value], file);
				if (nbytes != ovector[2 * value + 1] - ovector[2 * value]) {
					snprintf(f->buf_p, f->buf_size, "Failed to write match: %s", strerror(errno));
					goto err;
				}

				state = NORMAL;
				break;
			}
			break;
		case SEEN_BACKSLASH:
			value = 0;
			switch (delim[i]) {
			case 'a':
				if (putc('\a', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'b':
				if (putc('\b', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'f':
				if (putc('\f', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'n':
				if (putc('\n', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'r':
				if (putc('\r', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 't':
				if (putc('\t', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'v':
				if (putc('\v', file) == EOF)
					goto err_char;
				state = NORMAL;
				break;
			case 'x':
				value = 0;
				state = SEEN_HEX0;
				break;
			case '0': /* FALLTHROUGH */
			case '1': /* FALLTHROUGH */
			case '2': /* FALLTHROUGH */
			case '3': /* FALLTHROUGH */
			case '4': /* FALLTHROUGH */
			case '5': /* FALLTHROUGH */
			case '6': /* FALLTHROUGH */
			case '7':
				/* Note: may also be a backreference, handled below */
				value = delim[i] - '\0';
				state = SEEN_OCTAL1;
				break;
			case '8': /* FALLTHROUGH */
			case '9':
				value = delim[i] - '\0';
				goto output_match;
			default:
				snprintf(f->buf_p, f->buf_size, "Invalid escape sequence \\%c: reserved for future use", delim[i]);
				goto err;
			}
			break;
		case SEEN_HEX0: /* FALLTHROUGH */
		case SEEN_HEX1:
			if (isdigit(delim[i]))
				value = 16 * value + delim[i] - '0';
			else if (isxdigit(delim[i]))
				value = 16 * value + delim[i] - (isupper(delim[i]) ? 'A' : 'a') + 10;
			else if (state == SEEN_HEX1) {
				if (putc(value, file) == EOF)
					goto err_char;

				value = 0;
				state = NORMAL;
				i--;
			} else {
				snprintf(f->buf_p, f->buf_size, "Invalid escape sequence \\x%c: expected a hex digit", delim[i]);
				goto err;
			}

			if (state == SEEN_HEX0)
				state = SEEN_HEX1;
			else {
				if (putc(value, file) == EOF)
					goto err_char;

				value = 0;
				state = NORMAL;
			}
			break;
		case SEEN_OCTAL1: /* FALLTHROUGH */
		case SEEN_OCTAL2:
			if (isdigit(delim[i]) && delim[i] != '8' && delim[i] != '9')
				value = 8 * value + delim[i] - '0';
			else if (state == SEEN_OCTAL1 && value != 0) {
				/*
				 * This is not octal at all, but a
				 * backreference like "\2"!
				 */
				i--;
				goto output_match;
			} else {
				if (putc(value, file) == EOF)
					goto err_char;

				value = 0;
				state = NORMAL;
				i--;
			}

			if (state == SEEN_OCTAL1)
				state = SEEN_OCTAL2;
			else {
				if (putc(value, file) == EOF)
					goto err_char;

				value = 0;
				state = NORMAL;
			}
			break;
		}
	}

	if (value != 0 && putc(value, file) == EOF)
		goto err_char;

	assert(i == strlen(delim));

	return NULL;

err_char:
	snprintf(f->buf_p, f->buf_size, "Failed to write character: %s", strerror(errno));

err:
	return f->buf_p;
}
