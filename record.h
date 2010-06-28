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

/* XXX Description */
/* XXX Requires pcre.h, stdio.h */

#ifndef __GNUC__
#define __attribute__(x) /* Not supported by non-GCC compilers */
#endif

/*
 * Open a file for reading records.
 *
 * Returns a new struct rec_file on success; otherwise, returns NULL and sets
 * errno as for malloc(3) or mkstemp(3).
 */
struct rec_file;
struct rec_file *rec_open(int fd, pcre *re, pcre_extra *re_extra) __attribute__((nonnull(2, 3), malloc));

/*
 * Get location of next record.
 *
 * f and len must be non-NULL. If offset is non-NULL, it is set to the current
 * offset in the file; if p is on-NULL, it is pointed at a malloc()ed copy of
 * the record.
 *
 * Returns the size of the record on success; otherwise, returns -1 and sets
 * errno XXX. As a special case, it returns -1 and sets errno to 0 on EOF.
 */
int rec_next(struct rec_file *f, off_t *offset, char **p) __attribute__((nonnull(1)));

/*
 * Write record to FILE *.
 *
 * Set last to non-zero if this is the last record. delim XXX
 *
 * Returns 0 on success; otherwise, returns -1 and sets errno as for putc(3) or
 * fwrite(3).
 */
int rec_write_offset(struct rec_file *f, off_t offset, int len, int last, const char *delim, FILE *file) __attribute__((nonnull(1, 5, 6)));
int rec_write_mem(struct rec_file *f, const char *p, int len, int last, const char *delim, FILE *file) __attribute__((nonnull(1, 2, 5, 6)));

/*
 * Free all resources allocated by rec_open(). Note that this does not close
 * the file descriptor passed to rec_open().
 *
 * Returns 0 on succcess; otherwise, returns -1 and sets errno as for close(2).
 */
int rec_close(struct rec_file *f) __attribute__((nonnull(1)));

/*
 * Return the file record originally used to open this rec_file.
 */
int rec_fd(const struct rec_file *f) __attribute__((nonnull(1), pure));
