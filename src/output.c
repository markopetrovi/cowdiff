/*
 * output.c - say what was found.
 *
 * Binary files get byte ranges, which cost nothing beyond the work already
 * done to establish them.  Text files get a unified diff, which needs the
 * lines around each change: both as context, and because a hunk header names
 * line numbers, and a line number can only be had by counting the newlines
 * before it.  That count is the one cost this tool cannot avoid when asked
 * for diff-format text output.
 */
#define _GNU_SOURCE
#include "cowdiff.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Lines of context around each hunk, as in diff -U. */
static unsigned int context_lines = 3;

void text_diff_set_context(int n)
{
	context_lines = n < 0 ? 0u : (unsigned int)n;
}

/*
 * A ---/+++ header line, carrying the file's timestamp the way diff(1) does:
 * after a tab, as "YYYY-MM-DD HH:MM:SS.NNNNNNNNN +ZZZZ".  A drop-in should
 * put the same thing there, and a script that reads the header -- or a person
 * reading a diff -- expects it.  patch(1) ignores it either way.
 */
static void print_file_header(const char *path, int fd, const char *mark)
{
	struct stat st;
	struct tm tm;
	char date[32], zone[8];

	if (fstat(fd, &st) == 0 && localtime_r(&st.st_mtime, &tm) != NULL) {
		strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm);
		strftime(zone, sizeof zone, "%z", &tm);
		printf("%s %s\t%s.%09ld %s\n", mark, path, date,
		       (long)st.st_mtim.tv_nsec, zone);
	} else {
		printf("%s %s\n", mark, path);
	}
}

bool is_binary(int fd)
{
	unsigned char buf[4096];
	ssize_t n = pread(fd, buf, sizeof buf, 0);
	ssize_t i;

	if (n <= 0)
		return false;
	for (i = 0; i < n; i++)
		if (buf[i] == '\0')
			return true;
	return false;
}

/* ---- binary ----------------------------------------------------------- */

/* Called with at least one delta: compare_files() answers "identical" itself,
 * from an empty list, before it gets here. */
int emit_binary_diff(const char *path_a, const char *path_b,
		     const struct deltalist *dl)
{
	unsigned long long only_a = 0, only_b = 0, unread = 0;
	size_t i, nunread = 0;

	for (i = 0; i < dl->n; i++) {
		only_a += dl->v[i].a_len;
		only_b += dl->v[i].b_len;
		if (dl->v[i].unreadable) {
			nunread++;
			unread += dl->v[i].a_len;
		}
	}

	printf("Binary files %s and %s differ\n", path_a, path_b);
	printf("%zu differing region%s (%llu bytes in %s, %llu bytes in %s)\n",
	       dl->n, dl->n == 1 ? "" : "s", only_a, path_a, only_b, path_b);
	/*
	 * Said separately and in full, because "could not be read" is not the
	 * same finding as "holds different bytes": it is a difference only
	 * because equality could not be proven there, and whoever reads this
	 * needs to know which of the two they have.
	 */
	if (nunread)
		printf("%zu region%s (%llu bytes) could not be read: "
		       "input/output error, so not shown to match\n",
		       nunread, nunread == 1 ? "" : "s", unread);

	for (i = 0; i < dl->n; i++) {
		const struct delta *d = &dl->v[i];

		if (d->unreadable) {
			const char *where = d->unreadable == UNREAD_AB
						    ? "both files"
						    : (d->unreadable & UNREAD_A
							       ? path_a : path_b);

			if (d->a_off == d->b_off && d->a_len == d->b_len)
				printf("  unreadable  both at 0x%llx, %llu bytes (input/output error in %s)\n",
				       (unsigned long long)d->a_off,
				       (unsigned long long)d->a_len, where);
			else
				printf("  unreadable  %s[0x%llx, %llu bytes] -> %s[0x%llx, %llu bytes] (input/output error in %s)\n",
				       path_a, (unsigned long long)d->a_off,
				       (unsigned long long)d->a_len, path_b,
				       (unsigned long long)d->b_off,
				       (unsigned long long)d->b_len, where);
			continue;
		}

		if (d->a_len == 0)
			printf("  inserted  %s[0x%llx, %llu bytes]\n", path_b,
			       (unsigned long long)d->b_off,
			       (unsigned long long)d->b_len);
		else if (d->b_len == 0)
			printf("  deleted   %s[0x%llx, %llu bytes]\n", path_a,
			       (unsigned long long)d->a_off,
			       (unsigned long long)d->a_len);
		else if (d->a_off == d->b_off && d->a_len == d->b_len)
			printf("  changed   both at 0x%llx, %llu bytes\n",
			       (unsigned long long)d->a_off,
			       (unsigned long long)d->a_len);
		else
			printf("  changed   %s[0x%llx, %llu bytes] -> %s[0x%llx, %llu bytes]\n",
			       path_a, (unsigned long long)d->a_off,
			       (unsigned long long)d->a_len, path_b,
			       (unsigned long long)d->b_off,
			       (unsigned long long)d->b_len);
	}
	return 1;
}

/* ---- text ------------------------------------------------------------- */

/*
 * Newlines before an offset, scanning forward from wherever the previous
 * question left off.  Every offset asked about here is larger than the last,
 * so a run of hunks costs one pass over the file between them, not one pass
 * each.
 */
struct lcounter {
	int fd;
	uint64_t off;
	uint64_t lines;
	bool blind;		/* a bad block was crossed: the count is a
				 * lower bound, so no line number may be printed */
};

/*
 * The counting itself lives in count_newlines, which reads eight bytes per
 * step; this is only the reading and the running total.
 *
 * A block that cannot be read is skipped rather than refused: its newlines
 * cannot be counted, so the total stops being a count and becomes a lower
 * bound, which is recorded in `blind` and costs the caller its line numbers.
 */
static int lc_count(struct lcounter *lc, uint64_t off, uint64_t *out)
{
	unsigned char buf[64 * 1024];
	uint64_t lines = lc->lines;

	while (lc->off < off) {
		uint64_t want = off - lc->off < sizeof buf ? off - lc->off
							   : sizeof buf;

		if (pread_full(lc->fd, buf, want, lc->off) < 0) {
			if (errno != EIO)
				return -1;
			lc->blind = true;
			lc->off += want;
			continue;
		}
		lines += count_newlines(buf, want);
		lc->off += want;
	}
	lc->lines = lines;
	*out = lines;
	return 0;
}

/*
 * Output goes through a buffer of ours rather than straight into stdio.
 *
 * A hunk body is one call per line to fputc and one to fwrite, each of which
 * takes a lock, and on a shape whose answer is a million lines that dominated
 * the printing.  Assembling the lines here and handing stdio whole kilobytes
 * is several times quicker, and the lock is not needed at all: nothing else
 * in this program writes.
 */
#define OBUF_SIZE (256 * 1024)
static unsigned char obuf[OBUF_SIZE];
static size_t obuf_len;

static void obuf_flush(void)
{
	if (obuf_len) {
		fwrite_unlocked(obuf, 1, obuf_len, stdout);
		obuf_len = 0;
	}
}

static void obuf_put(const void *p, size_t n)
{
	const unsigned char *s = p;

	if (n > OBUF_SIZE) {
		obuf_flush();
		fwrite_unlocked(s, 1, n, stdout);
		return;
	}
	if (obuf_len + n > OBUF_SIZE)
		obuf_flush();
	memcpy(obuf + obuf_len, s, n);
	obuf_len += n;
}

static void obuf_putc(char c)
{
	if (obuf_len == OBUF_SIZE)
		obuf_flush();
	obuf[obuf_len++] = (unsigned char)c;
}

static const char no_newline[] = "\n\\ No newline at end of file\n";

static int print_lines(int fd, uint64_t off, uint64_t end, char prefix)
{
	unsigned char rbuf[64 * 1024];
	unsigned char *line = NULL;
	size_t lcap = 0, llen = 0;
	uint64_t p = off;
	int rc = -1;

	while (p < end) {
		uint64_t want = end - p < sizeof rbuf ? end - p : sizeof rbuf;
		uint64_t i = 0;

		if (pread_full(fd, rbuf, want, p) < 0)
			goto out;

		while (i < want) {
			unsigned char *nl = memchr(rbuf + i, '\n',
						   (size_t)(want - i));
			size_t n = nl ? (size_t)(nl - (rbuf + i)) + 1
				      : (size_t)(want - i);

			if (llen == 0 && nl) {
				/* Whole lines in this chunk: straight through. */
				obuf_putc(prefix);
				obuf_put(rbuf + i, n);
			} else {
				/* A line crossing a chunk boundary, or the
				 * unterminated last one: gather it first. */
				if (llen + n > lcap) {
					size_t ncap = lcap ? lcap : 256;
					unsigned char *nline;

					while (ncap < llen + n)
						ncap *= 2;
					nline = realloc(line, ncap);
					if (!nline)
						goto out;
					line = nline;
					lcap = ncap;
				}
				memcpy(line + llen, rbuf + i, n);
				llen += n;
				if (nl) {
					obuf_putc(prefix);
					obuf_put(line, llen);
					llen = 0;
				}
			}
			i += n;
		}
		p += want;
	}

	/* A range that stops without a newline stopped at end of file. */
	if (llen > 0) {
		obuf_putc(prefix);
		obuf_put(line, llen);
		obuf_put(no_newline, sizeof no_newline - 1);
	}

	/* Everything buffered here has to reach stdout before the caller
	 * prints the next hunk header through stdio. */
	obuf_flush();
	rc = 0;
out:
	free(line);
	return rc;
}

/* Step back n lines, reporting how many were actually available. */
static int rewind_lines(int fd, uint64_t off, unsigned int n, uint64_t *out,
			unsigned int *moved)
{
	unsigned int k = 0;

	while (k < n && off > 0) {
		uint64_t s;

		/* off is a line start, so off-1 is the newline that ended the
		 * line before it. */
		if (line_start(fd, off - 1, &s) < 0)
			return -1;
		if (s >= off)
			break;
		off = s;
		k++;
	}
	*out = off;
	*moved = k;
	return 0;
}

/*
 * Step forward n lines, reporting how many *newlines* the walk crossed.
 *
 * That is not the number of lines it entered.  The last line of a file need
 * not end in a newline, so a walk that stops there has entered a line without
 * crossing one -- and callers count lines as newline counts, which is why the
 * difference matters: `crossed` is added to the newline count taken before
 * the walk, and the caller then adds one more for a final unterminated line.
 * Counting the step as well added that correction twice, and a hunk header
 * claiming one line more than its body carries is a hunk patch(1) refuses.
 */
static int forward_lines(int fd, uint64_t off, uint64_t limit, unsigned int n,
			 uint64_t *out, unsigned int *crossed)
{
	unsigned int k = 0, nl = 0;

	while (k < n && off < limit) {
		uint64_t e;
		bool found;

		if (line_end(fd, off, limit, &e, &found) < 0)
			return -1;
		if (e <= off)
			break;
		off = e;
		k++;
		if (found)
			nl++;
	}
	*out = off;
	*crossed = nl;
	return 0;
}

/* A byte range that differs, snapped out to whole lines on both sides. */
struct bedit {
	uint64_t a_off, a_end;
	uint64_t b_off, b_end;
	uint64_t a_line, a_end_line;	/* newlines before each offset */
	uint64_t b_line, b_end_line;
};

struct beditlist {
	struct bedit *v;
	size_t n, cap;
};

static int bedit_push(struct beditlist *bl, struct bedit e)
{
	if (bl->n) {
		struct bedit *last = &bl->v[bl->n - 1];

		/* Overlapping once snapped to lines: extend rather than add. */
		if (e.a_off <= last->a_end && e.b_off <= last->b_end) {
			if (e.a_end > last->a_end) {
				last->a_end = e.a_end;
				last->a_end_line = e.a_end_line;
			}
			if (e.b_end > last->b_end) {
				last->b_end = e.b_end;
				last->b_end_line = e.b_end_line;
			}
			return 0;
		}
	}

	if (bl->n == bl->cap) {
		size_t ncap = bl->cap ? bl->cap * 2 : 32;
		struct bedit *nv = realloc(bl->v, ncap * sizeof *nv);

		if (!nv)
			return -1;
		bl->v = nv;
		bl->cap = ncap;
	}
	bl->v[bl->n++] = e;
	return 0;
}

/* ---- trimming a delta down to the part that differs -------------------- */

/*
 * A delta whose two sides differ in length never had its bytes compared: the
 * gap resolver reports such a span as replaced without reading it, because
 * for binary output that is already the honest answer.  For text output the
 * line diff is then handed both whole files -- on the benchmark's
 * one-changed-line shape that is 66% of the runtime spent reading and hashing
 * 70 MB to find a difference in the last line of one of them.
 *
 * So compare what the byte level did not: strip the common prefix and suffix
 * before building the linesets, and let the line diff see only the region
 * that actually differs.
 *
 * The boundaries have to be handled carefully.  Each side has to be cut at
 * one of its own line boundaries, or the lineset would begin or end inside a
 * line; and *both* sides have to give up the same number of bytes, or the
 * lines left outside the two regions would no longer be the same lines and
 * the diff would report trailing lines as deleted that are not.  The prefix
 * is safe on both counts because its bytes are equal, so a line boundary in
 * one file is one in the other; the suffix is cut back to the first whole
 * line in it for the same reason.
 */
/* The first line start at or after `off`, or `end` if there is none. */
static int line_start_from(int fd, uint64_t off, uint64_t end, uint64_t *out)
{
	unsigned char buf[16 * 1024];
	uint64_t p = off;

	while (p < end) {
		uint64_t want = end - p < sizeof buf ? end - p : sizeof buf;
		unsigned char *nl;

		if (pread_full(fd, buf, want, p) < 0)
			return -1;
		nl = memchr(buf, '\n', want);
		if (nl) {
			*out = p + (uint64_t)(nl - buf) + 1;
			return 0;
		}
		p += want;
	}
	*out = end;
	return 0;
}

/*
 * Shrink a delta's two line-snapped spans to the part that differs, in place.
 * Never grows them, and both sides always lose the same number of bytes.
 */
static int span_trim(int fd_a, uint64_t *alo, uint64_t *ahi,
		     int fd_b, uint64_t *blo, uint64_t *bhi)
{
	uint64_t alen = *ahi - *alo, blen = *bhi - *blo;
	uint64_t limit = alen < blen ? alen : blen;
	uint64_t kp = 0, ks = 0, x;

	if (common_prefix(fd_a, *alo, fd_b, *blo, limit, &kp) < 0)
		return -1;
	if (kp) {
		/* Back to the start of the line the first difference is on. */
		if (line_start(fd_a, *alo + kp, &x) < 0)
			return -1;
		kp = x - *alo;
	}

	if (common_suffix(fd_a, *ahi, fd_b, *bhi, limit - kp, &ks) < 0)
		return -1;
	if (ks) {
		/* Forward to the first whole line of the common suffix. */
		if (line_start_from(fd_a, *ahi - ks, *ahi, &x) < 0)
			return -1;
		ks = *ahi - x;
	}

	*alo += kp;
	*blo += kp;
	*ahi -= ks;
	*bhi -= ks;
	return 0;
}

/* Map a line index back to a byte offset within a lineset. */
static uint64_t ls_off(const struct lineset *ls, uint64_t base, size_t i)
{
	return i < ls->n ? base + ls->v[i].boff : base + ls->buflen;
}

/* Emit the lines of a hunk: context, removals, insertions, context. */
static int print_hunk_body(int fd_a, int fd_b, const struct beditlist *bl,
			   size_t first, size_t last, uint64_t a_lo,
			   uint64_t a_hi)
{
	uint64_t cursor = a_lo;
	size_t i;

	for (i = first; i <= last; i++) {
		const struct bedit *e = &bl->v[i];

		if (print_lines(fd_a, cursor, e->a_off, ' ') < 0)
			return -1;
		if (print_lines(fd_a, e->a_off, e->a_end, '-') < 0)
			return -1;
		if (print_lines(fd_b, e->b_off, e->b_end, '+') < 0)
			return -1;
		cursor = e->a_end;
	}
	return print_lines(fd_a, cursor, a_hi, ' ');
}

static bool ends_with_newline(int fd, uint64_t off)
{
	unsigned char c;

	if (off == 0)
		return true;
	if (pread_full(fd, &c, 1, off - 1) < 0)
		return true;
	return c == '\n';
}

static int emit_hunk(int fd_a, int fd_b, uint64_t size_a,
		     const struct beditlist *bl, size_t first, size_t last)
{
	const struct bedit *e0 = &bl->v[first];
	const struct bedit *e1 = &bl->v[last];
	uint64_t a_lo, a_hi, b_lo, b_hi;
	uint64_t old_start, old_count, new_start, new_count;
	unsigned int back = 0, fwd_nl = 0;

	if (rewind_lines(fd_a, e0->a_off, context_lines, &a_lo, &back) < 0)
		return -1;
	if (forward_lines(fd_a, e1->a_end, size_a, context_lines, &a_hi,
			  &fwd_nl) < 0)
		return -1;

	/*
	 * Context is identical in both files, so B's hunk starts the same
	 * distance before its first edit as A's does, and ends after the
	 * same number of trailing lines.
	 */
	b_lo = e0->b_off - (e0->a_off - a_lo);
	b_hi = e1->b_end + (a_hi - e1->a_end);

	/*
	 * Line numbers are newline counts, but a final line with no newline
	 * is still a line, and diff(1) counts it as one.  `fwd_nl` counts
	 * newlines crossed, not lines stepped onto, so that the correction
	 * below is added exactly once whether or not the trailing context
	 * reaches the unterminated end of the file.
	 */
	old_start = e0->a_line - back;
	old_count = e1->a_end_line + fwd_nl - old_start;
	new_start = e0->b_line - back;
	new_count = e1->b_end_line + fwd_nl - new_start;
	if (a_hi > a_lo && !ends_with_newline(fd_a, a_hi))
		old_count++;
	if (b_hi > b_lo && !ends_with_newline(fd_b, b_hi))
		new_count++;

	/* diff(1) writes a zero-length side as "<line before>,0", and drops
	 * the count altogether when it is exactly 1. */
	printf("@@ -%llu", (unsigned long long)(old_count ? old_start + 1
							 : old_start));
	if (old_count != 1)
		printf(",%llu", (unsigned long long)old_count);
	printf(" +%llu", (unsigned long long)(new_count ? new_start + 1
						       : new_start));
	if (new_count != 1)
		printf(",%llu", (unsigned long long)new_count);
	printf(" @@\n");

	return print_hunk_body(fd_a, fd_b, bl, first, last, a_lo, a_hi);
}

/*
 * The same hunk, but the header carries byte offsets instead of line numbers.
 *
 * Line numbers are the only reason the text path has to read anything at all
 * once the extent map has proven most of the file equal: a line number is a
 * count of newlines from the start of the file, so producing one costs a pass
 * over the whole thing.  A caller that does not need them keeps the entire
 * benefit of knowing which bytes are shared.
 *
 * Both numbers are always printed, including a zero length, because unlike
 * the line form there is no "count of one may be omitted" convention to lean
 * on and a bare offset would read as a line number.
 */
static int emit_hunk_bytes(int fd_a, int fd_b, uint64_t size_a,
			   const struct beditlist *bl, size_t first,
			   size_t last)
{
	const struct bedit *e0 = &bl->v[first];
	const struct bedit *e1 = &bl->v[last];
	uint64_t a_lo, a_hi, b_lo, b_hi;
	unsigned int back = 0, fwd_nl = 0;

	if (rewind_lines(fd_a, e0->a_off, context_lines, &a_lo, &back) < 0)
		return -1;
	if (forward_lines(fd_a, e1->a_end, size_a, context_lines, &a_hi,
			  &fwd_nl) < 0)
		return -1;

	/* Only the byte offsets are printed here, so neither count is used. */
	b_lo = e0->b_off - (e0->a_off - a_lo);
	b_hi = e1->b_end + (a_hi - e1->a_end);

	printf("@@ -%llu,%llu +%llu,%llu @@\n", (unsigned long long)a_lo,
	       (unsigned long long)(a_hi - a_lo), (unsigned long long)b_lo,
	       (unsigned long long)(b_hi - b_lo));

	return print_hunk_body(fd_a, fd_b, bl, first, last, a_lo, a_hi);
}

/*
 * Would two edits be close enough to share a hunk?  In line mode that is a
 * subtraction of two line numbers, which only exist after the whole-file
 * scan.  Here the same question is answered by counting the newlines in the
 * equal region between them and giving up as soon as there are too many --
 * and giving up on bytes scanned as well, so that a region with no newlines
 * in it at all cannot turn into an unbounded read.
 */
#define GAP_SCAN_BUDGET (1u << 20)

static bool gap_is_close(int fd, uint64_t from, uint64_t to)
{
	unsigned char buf[4096];
	uint64_t p = from, spent = 0;
	unsigned int nl = 0;

	while (p < to && spent < GAP_SCAN_BUDGET) {
		uint64_t want = to - p;
		uint64_t i;

		if (want > sizeof buf)
			want = sizeof buf;
		if (want > GAP_SCAN_BUDGET - spent)
			want = GAP_SCAN_BUDGET - spent;
		if (pread_full(fd, buf, want, p) < 0)
			return false;
		for (i = 0; i < want; i++)
			if (buf[i] == '\n' && ++nl > 2 * context_lines)
				return false;
		p += want;
		spent += want;
	}
	return p >= to;
}

/*
 * The one line boundary most recently asked about in a file.
 *
 * Every delta asks where the lines its two byte ranges fall on start and end,
 * and each of those questions reads out to the nearest newline on its own.
 * Almost always that is a few bytes, and the answer is the same one over and
 * over when several edits land on the same line -- but a file whose lines are
 * megabytes long, or which has no newlines in it at all (one long line is a
 * shape that exists: minified JSON, a file with CR-only line endings), makes
 * each question a walk over the rest of the file, and asking it per delta
 * makes the run quadratic in what it reads instead of linear in the file.
 *
 * So the last line found is kept: a question about an offset on it is free,
 * and a question about an offset elsewhere is asked the ordinary way and
 * seeds the cache with *its* line.  That way an edit-dense file stops paying
 * for its line lengths, an edit-sparse one pays exactly what it did before,
 * and neither gets a wrong answer to save time.
 */
struct lcache {
	int fd;
	uint64_t size;
	uint64_t line;		/* start of the line we know */
	uint64_t end;		/* end of it: past its newline, or the size */
	bool terminated;	/* that end is a newline, not the end of file */
	bool valid;
};

/*
 * The line containing `off`: where it starts, and where it ends.  Exactly what
 * line_start() and line_end() answer between them.
 */
static int lcache_at(struct lcache *c, uint64_t off, uint64_t *start,
		     uint64_t *end)
{
	/*
	 * On the line we know.  A line that ends at the end of the file holds
	 * that position as well; a line that ends in a newline does not, since
	 * the position after it is the start of the next line.
	 */
	if (c->valid && off >= c->line &&
	    (off < c->end || (off == c->end && !c->terminated))) {
		*start = c->line;
		*end = c->end;
		return 0;
	}

	{
		uint64_t s, e;
		bool nl;

		if (line_start(c->fd, off, &s) < 0)
			return -1;
		if (line_end(c->fd, off, c->size, &e, &nl) < 0)
			return -1;

		c->line = s;
		c->end = e;
		c->terminated = nl;
		c->valid = true;
		*start = s;
		*end = e;
		return 0;
	}
}

/* line_start() and line_end() through the cache. */
static int lc_start(struct lcache *c, uint64_t off, uint64_t *out)
{
	uint64_t ignore;

	return lcache_at(c, off, out, &ignore);
}

static int lc_end(struct lcache *c, uint64_t off, uint64_t *out)
{
	uint64_t ignore;

	return lcache_at(c, off, &ignore, out);
}

/*
 * Turn one byte range that differs into line ranges: snap it out to whole
 * lines and let the line diff refine it.  Returns 0, or -1 with errno set --
 * EIO meaning part of the range could not be read, which the caller reports
 * as an unreadable region rather than dying on.
 *
 * The two caches belong to the caller, one per file, and are only useful
 * because the deltas arrive in ascending order (see struct lcache).
 */
static int range_to_bedits(struct beditlist *bl, int fd_a, int fd_b,
			   struct lcache *ca, struct lcache *cb,
			   uint64_t a_off, uint64_t a_len,
			   uint64_t b_off, uint64_t b_len)
{
	struct bedit e;
	struct lineset la, lb;
	struct editlist el;
	size_t k;

	if (lc_start(ca, a_off, &e.a_off) < 0 ||
	    lc_end(ca, a_off + a_len, &e.a_end) < 0 ||
	    lc_start(cb, b_off, &e.b_off) < 0 ||
	    lc_end(cb, b_off + b_len, &e.b_end) < 0)
		return -1;

	/*
	 * A delta that came from comparing the same span of both files already
	 * carries its own alignment: the bytes differ there and agree
	 * everywhere else, so the changed lines are simply whatever those
	 * bytes fall on.  Running a line diff would only rediscover that --
	 * and on a file with many scattered changes the rediscovery is the
	 * entire cost, because the search reads the whole range at every level
	 * of its recursion.
	 *
	 * Only a delta of the same length at the same offset can be trusted
	 * this way.  A pure insertion or deletion, or content that moved, is
	 * exactly the case where the alignment is *not* known and the search
	 * is what finds it.
	 */
	if (a_len == b_len && a_off == b_off)
		return bedit_push(bl, e);

	/*
	 * Everything else arrives here without its bytes having been compared,
	 * so work out how much of the two spans is actually common before
	 * reading them in full.
	 */
	if (span_trim(fd_a, &e.a_off, &e.a_end, fd_b, &e.b_off, &e.b_end) < 0)
		return -1;

	if (lineset_build(&la, fd_a, e.a_off, e.a_end - e.a_off) < 0)
		return -1;
	if (lineset_build(&lb, fd_b, e.b_off, e.b_end - e.b_off) < 0) {
		lineset_free(&la);
		return -1;
	}
	if (linediff(&la, &lb, &el) < 0) {
		lineset_free(&la);
		lineset_free(&lb);
		return -1;
	}

	for (k = 0; k < el.n; k++) {
		struct bedit r;

		r.a_off = ls_off(&la, e.a_off, el.v[k].alo);
		r.a_end = ls_off(&la, e.a_off, el.v[k].ahi);
		r.b_off = ls_off(&lb, e.b_off, el.v[k].blo);
		r.b_end = ls_off(&lb, e.b_off, el.v[k].bhi);
		r.a_line = r.a_end_line = r.b_line = r.b_end_line = 0;

		if (bedit_push(bl, r) < 0) {
			edits_free(&el);
			lineset_free(&la);
			lineset_free(&lb);
			return -1;
		}
	}

	edits_free(&el);
	lineset_free(&la);
	lineset_free(&lb);
	return 0;
}

int emit_text_diff(const char *path_a, const char *path_b, int fd_a, int fd_b,
		   const struct extmap *ma, const struct extmap *mb,
		   const struct deltalist *dl, bool byte_offsets)
{
	struct beditlist bl;
	struct lcounter ca, cb;
	struct lcache lca, lcb;
	struct range { uint64_t off, end; } *bad = NULL;
	size_t nbad = 0, cap_bad = 0;
	char *touches = NULL;
	size_t nunread = 0;
	size_t i, first;
	bool header_done = false;
	int rc = -1;

	memset(&bl, 0, sizeof bl);
	memset(&ca, 0, sizeof ca);
	memset(&cb, 0, sizeof cb);
	ca.fd = fd_a;
	cb.fd = fd_b;
	memset(&lca, 0, sizeof lca);		/* line-boundary caches, not the
						 * newline counters above */
	memset(&lcb, 0, sizeof lcb);
	lca.fd = fd_a;
	lca.size = ma->size;
	lcb.fd = fd_b;
	lcb.size = mb->size;

	if (dl->n == 0)
		return 0;

	/*
	 * Snap each differing byte range out to whole lines, then let the line
	 * diff refine it: a byte range can cut a line in half, and a replaced
	 * block is often far smaller at line granularity than a byte-level
	 * comparison can tell.
	 */
	/*
	 * Which deltas can go through the line diff at all.  A range that could
	 * not be read has no lines to diff -- snapping it to line boundaries
	 * means reading around it, let alone splitting it into lines -- and
	 * neither has a delta that merely *overlaps* one, since the line diff
	 * reads its whole span and the unreadable block sits inside it.  Both
	 * kinds are reported afterwards in byte offsets, where they cannot be
	 * mistaken for a line-level finding.
	 */
	if (dl->n) {
		size_t k = 0;

		for (i = 0; i < dl->n; i++) {
			const struct delta *d = &dl->v[i];

			if (d->unreadable) {
				if (nbad == cap_bad) {
					size_t ncap = cap_bad ? cap_bad * 2 : 64;
					struct range *nv = realloc(bad,
							ncap * sizeof *nv);
					if (!nv)
						goto out;
					bad = nv;
					cap_bad = ncap;
				}
				bad[nbad].off = d->a_off;
				bad[nbad].end = d->a_off + d->a_len;
				nbad++;
			}
		}
		touches = calloc(dl->n, 1);
		if (!touches)
			goto out;
		for (i = 0; i < dl->n; i++) {
			const struct delta *d = &dl->v[i];
			uint64_t o = d->a_off, e = d->a_off + d->a_len;

			while (k < nbad && bad[k].end <= o)
				k++;
			if (k < nbad && bad[k].off < e)
				touches[i] = 1;
		}
	}

	for (i = 0; i < dl->n; i++) {
		const struct delta *d = &dl->v[i];
		int rc2;

		if (touches[i])
			continue;         /* reported in bytes, below */

		rc2 = range_to_bedits(&bl, fd_a, fd_b, &lca, &lcb, d->a_off,
				      d->a_len, d->b_off, d->b_len);
		if (rc2 < 0) {
			if (errno != EIO)
				goto out;
			/*
			 * A block inside this range could not be read, and the byte
			 * level never saw it -- a delta whose two sides differ in
			 * length is reported without being compared, and reading
			 * around it to find line boundaries is what failed.  The
			 * range is reported as unreadable rather than diffed: what
			 * is left of it cannot be shown to match either, and the
			 * line diff cannot span a gap whose contents are unknown.
			 */
			touches[i] = 1;
		}
	}

	for (i = 0; i < dl->n; i++)
		if (touches[i])
			nunread++;

	if (bl.n == 0 && nunread == 0) {
		rc = 0;
		goto out;
	}

	if (!byte_offsets) {
		/*
		 * Line numbers, in one forward pass per file.  Edits are in
		 * order and do not overlap, so the counter never has to go
		 * backwards.  This is the pass that byte offsets avoid.
		 */
		for (i = 0; i < bl.n; i++) {
			if (lc_count(&ca, bl.v[i].a_off, &bl.v[i].a_line) < 0 ||
			    lc_count(&ca, bl.v[i].a_end,
				     &bl.v[i].a_end_line) < 0 ||
			    lc_count(&cb, bl.v[i].b_off, &bl.v[i].b_line) < 0 ||
			    lc_count(&cb, bl.v[i].b_end,
				     &bl.v[i].b_end_line) < 0)
				goto out;
		}

		/*
		 * A line number is a count of newlines from the start of the
		 * file, and a block that cannot be read cannot be counted.  The
		 * numbers from there on would be quietly wrong, which is the
		 * one thing an output like this must not be, so the hunks are
		 * given byte offsets instead -- the numbers a reader can trust
		 * -- and the reason is said out loud.
		 */
		if (ca.blind || cb.blind) {
			fprintf(stderr,
				"cowdiff: could not read all of %s; "
				"line numbers would be wrong, so hunk headers "
				"carry byte offsets\n",
				ca.blind ? path_a : path_b);
			byte_offsets = true;
		}
	}

	/*
	 * Group into hunks the way diff does: an edit within twice the
	 * context of its neighbour belongs in the same hunk.
	 */
	first = 0;
	for (i = 0; i <= bl.n; i++) {
		bool flush = (i == bl.n);

		if (i > first && i < bl.n) {
			if (byte_offsets) {
				if (!gap_is_close(fd_a, bl.v[i - 1].a_end,
						  bl.v[i].a_off))
					flush = true;
			} else if (bl.v[i].a_line - bl.v[i - 1].a_end_line >
				   2 * context_lines) {
				flush = true;
			}
		}

		if (!flush)
			continue;

		if (!header_done) {
			print_file_header(path_a, fd_a, "---");
			print_file_header(path_b, fd_b, "+++");
			header_done = true;
		}
		if (i > first) {
			int r = byte_offsets
					? emit_hunk_bytes(fd_a, fd_b, ma->size,
							  &bl, first, i - 1)
					: emit_hunk(fd_a, fd_b, ma->size, &bl,
						    first, i - 1);

			if (r < 0) {
				if (errno != EIO)
					goto out;
				printf("cowdiff: unreadable context at "
				       "0x%llx -- input/output error, hunk "
				       "not shown\n",
				       (unsigned long long)bl.v[first].a_off);
			}
		}
		first = i;
	}

	/*
	 * Regions that could not be read, last and plainly marked.  They are
	 * differences because equality there was not established, not because
	 * anything was seen to differ, and the line below deliberately is not
	 * a diff line: an output that quietly omitted a region, or that looked
	 * patchable while missing one, would be worse than an obvious note
	 * that it is not.
	 */
	if (nunread) {
		if (!header_done) {
			print_file_header(path_a, fd_a, "---");
			print_file_header(path_b, fd_b, "+++");
			header_done = true;
		}
		for (i = 0; i < dl->n; i++) {
			const struct delta *d = &dl->v[i];

			if (!touches[i])
				continue;
			printf("cowdiff: unreadable %s[0x%llx, %llu bytes] vs "
			       "%s[0x%llx, %llu bytes] -- input/output error, "
			       "so not shown to match%s\n",
			       path_a, (unsigned long long)d->a_off,
			       (unsigned long long)d->a_len, path_b,
			       (unsigned long long)d->b_off,
			       (unsigned long long)d->b_len,
			       d->unreadable == UNREAD_AB ? " (neither side)"
			       : d->unreadable ? " (one side)" : "");
		}
		fprintf(stderr,
			"cowdiff: %zu region%s could not be read (input/output "
			"error); they are reported as differences, and no diff "
			"line describes them, so this output is not a patch of "
			"the whole difference\n",
			nunread, nunread == 1 ? "" : "s");
	}

	rc = header_done ? 1 : 0;
out:
	free(bad);
	free(touches);
	free(bl.v);
	return rc;
}
