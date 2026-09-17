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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONTEXT 3

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

int emit_binary_diff(const char *path_a, const char *path_b,
		     const struct deltalist *dl)
{
	unsigned long long only_a = 0, only_b = 0;
	size_t i;

	if (dl->n == 0) {
		printf("Files %s and %s are identical\n", path_a, path_b);
		return 0;
	}

	for (i = 0; i < dl->n; i++) {
		only_a += dl->v[i].a_len;
		only_b += dl->v[i].b_len;
	}

	printf("Binary files %s and %s differ\n", path_a, path_b);
	printf("%zu differing region%s (%llu bytes in %s, %llu bytes in %s)\n",
	       dl->n, dl->n == 1 ? "" : "s", only_a, path_a, only_b, path_b);

	for (i = 0; i < dl->n; i++) {
		const struct delta *d = &dl->v[i];

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
};

static int lc_count(struct lcounter *lc, uint64_t off, uint64_t *out)
{
	unsigned char buf[64 * 1024];

	while (lc->off < off) {
		uint64_t want = off - lc->off < sizeof buf ? off - lc->off
							   : sizeof buf;
		uint64_t i;

		if (pread_full(lc->fd, buf, want, lc->off) < 0)
			return -1;
		for (i = 0; i < want; i++)
			if (buf[i] == '\n')
				lc->lines++;
		lc->off += want;
	}
	*out = lc->lines;
	return 0;
}

static int print_lines(int fd, uint64_t off, uint64_t end, char prefix)
{
	unsigned char rbuf[64 * 1024];
	unsigned char *line = NULL;
	size_t lcap = 0, llen = 0;
	uint64_t p = off;
	int rc = -1;

	while (p < end) {
		uint64_t want = end - p < sizeof rbuf ? end - p : sizeof rbuf;
		uint64_t i;

		if (pread_full(fd, rbuf, want, p) < 0)
			goto out;
		for (i = 0; i < want; i++) {
			if (llen == lcap) {
				size_t ncap = lcap ? lcap * 2 : 256;
				unsigned char *nl = realloc(line, ncap);

				if (!nl)
					goto out;
				line = nl;
				lcap = ncap;
			}
			line[llen++] = rbuf[i];
			if (rbuf[i] == '\n') {
				fputc(prefix, stdout);
				fwrite(line, 1, llen, stdout);
				llen = 0;
			}
		}
		p += want;
	}

	/* A range that stops without a newline stopped at end of file. */
	if (llen > 0) {
		fputc(prefix, stdout);
		fwrite(line, 1, llen, stdout);
		fputs("\n\\ No newline at end of file\n", stdout);
	}

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

/* Step forward n lines, reporting how many were actually available. */
static int forward_lines(int fd, uint64_t off, uint64_t limit, unsigned int n,
			 uint64_t *out, unsigned int *moved)
{
	unsigned int k = 0;

	while (k < n && off < limit) {
		uint64_t e;

		if (line_end(fd, off, limit, &e) < 0)
			return -1;
		if (e <= off)
			break;
		off = e;
		k++;
	}
	*out = off;
	*moved = k;
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
	unsigned int back = 0, fwd = 0;

	if (rewind_lines(fd_a, e0->a_off, CONTEXT, &a_lo, &back) < 0)
		return -1;
	if (forward_lines(fd_a, e1->a_end, size_a, CONTEXT, &a_hi, &fwd) < 0)
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
	 * is still a line, and diff(1) counts it as one.
	 */
	old_start = e0->a_line - back;
	old_count = e1->a_end_line + fwd - old_start;
	new_start = e0->b_line - back;
	new_count = e1->b_end_line + fwd - new_start;
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
	unsigned int back = 0, fwd = 0;

	if (rewind_lines(fd_a, e0->a_off, CONTEXT, &a_lo, &back) < 0)
		return -1;
	if (forward_lines(fd_a, e1->a_end, size_a, CONTEXT, &a_hi, &fwd) < 0)
		return -1;

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
			if (buf[i] == '\n' && ++nl > 2 * CONTEXT)
				return false;
		p += want;
		spent += want;
	}
	return p >= to;
}

int emit_text_diff(const char *path_a, const char *path_b, int fd_a, int fd_b,
		   const struct extmap *ma, const struct extmap *mb,
		   const struct deltalist *dl, bool byte_offsets)
{
	struct beditlist bl;
	struct lcounter ca, cb;
	size_t i, first;
	bool header_done = false;
	int rc = -1;

	memset(&bl, 0, sizeof bl);
	ca.fd = fd_a;
	ca.off = 0;
	ca.lines = 0;
	cb.fd = fd_b;
	cb.off = 0;
	cb.lines = 0;

	if (dl->n == 0)
		return 0;

	/*
	 * Snap each differing byte range out to whole lines, then let the line
	 * diff refine it: a byte range can cut a line in half, and a replaced
	 * block is often far smaller at line granularity than a byte-level
	 * comparison can tell.
	 */
	for (i = 0; i < dl->n; i++) {
		const struct delta *d = &dl->v[i];
		struct bedit e;
		struct lineset la, lb;
		struct editlist el;
		size_t k;

		if (line_start(fd_a, d->a_off, &e.a_off) < 0 ||
		    line_end(fd_a, d->a_off + d->a_len, ma->size, &e.a_end) < 0 ||
		    line_start(fd_b, d->b_off, &e.b_off) < 0 ||
		    line_end(fd_b, d->b_off + d->b_len, mb->size, &e.b_end) < 0)
			goto out;

		if (lineset_build(&la, fd_a, e.a_off, e.a_end - e.a_off) < 0)
			goto out;
		if (lineset_build(&lb, fd_b, e.b_off, e.b_end - e.b_off) < 0) {
			lineset_free(&la);
			goto out;
		}
		if (linediff(&la, &lb, &el) < 0) {
			lineset_free(&la);
			lineset_free(&lb);
			goto out;
		}

		for (k = 0; k < el.n; k++) {
			struct bedit r;

			r.a_off = ls_off(&la, e.a_off, el.v[k].alo);
			r.a_end = ls_off(&la, e.a_off, el.v[k].ahi);
			r.b_off = ls_off(&lb, e.b_off, el.v[k].blo);
			r.b_end = ls_off(&lb, e.b_off, el.v[k].bhi);
			r.a_line = r.a_end_line = r.b_line = r.b_end_line = 0;

			if (bedit_push(&bl, r) < 0) {
				edits_free(&el);
				lineset_free(&la);
				lineset_free(&lb);
				goto out;
			}
		}

		edits_free(&el);
		lineset_free(&la);
		lineset_free(&lb);
	}

	if (bl.n == 0) {
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
				   2 * CONTEXT) {
				flush = true;
			}
		}

		if (!flush)
			continue;

		if (!header_done) {
			printf("--- %s\n+++ %s\n", path_a, path_b);
			header_done = true;
		}
		if (i > first) {
			int r = byte_offsets
					? emit_hunk_bytes(fd_a, fd_b, ma->size,
							  &bl, first, i - 1)
					: emit_hunk(fd_a, fd_b, ma->size, &bl,
						    first, i - 1);

			if (r < 0)
				goto out;
		}
		first = i;
	}

	rc = header_done ? 1 : 0;
out:
	free(bl.v);
	return rc;
}
