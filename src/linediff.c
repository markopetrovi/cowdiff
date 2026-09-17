/*
 * linediff.c - turn byte ranges that differ into line ranges that differ.
 *
 * By the time anything gets here the byte-level work is done: these ranges
 * are known to differ, and they are usually small, because everything the
 * filesystem could vouch for was already taken out as anchors.  So the
 * common case is a handful of lines and gets an exact answer.
 *
 * For the case where no sharing exists at all -- two ordinary files that were
 * written independently -- the range is the whole file, and an exact O(n*m)
 * answer is out of reach.  That falls back to splitting the range at a line
 * that occurs exactly once on each side, which is what makes ordinary text
 * line up; failing even that, the range is reported as wholly replaced, which
 * is coarse but never wrong.
 */
#include "cowdiff.h"

#include <stdlib.h>
#include <string.h>

/* Ranges up to this many lines on both sides are diffed exactly. */
#define SMALL 256

static uint64_t hash_line(const unsigned char *p, size_t len)
{
	uint64_t h = 1469598103934665603ULL;

	while (len--) {
		h ^= *p++;
		h *= 1099511628211ULL;
	}
	return h;
}

void lineset_free(struct lineset *ls)
{
	free(ls->buf);
	free(ls->v);
	memset(ls, 0, sizeof *ls);
}

int lineset_build(struct lineset *ls, int fd, uint64_t off, uint64_t len)
{
	size_t nlines = 0, i, k = 0, start = 0;

	memset(ls, 0, sizeof *ls);
	if (len == 0)
		return 0;

	ls->buf = malloc(len);
	if (!ls->buf)
		return -1;
	if (pread_full(fd, ls->buf, len, off) < 0) {
		lineset_free(ls);
		return -1;
	}
	ls->buflen = len;

	for (i = 0; i < len; i++)
		if (ls->buf[i] == '\n')
			nlines++;
	/* A trailing fragment with no newline is still a line -- and it is a
	 * *different* line from the same text with a newline, which is how
	 * diff reports a missing newline at end of file. */
	if (ls->buf[len - 1] != '\n')
		nlines++;

	ls->v = malloc((nlines ? nlines : 1) * sizeof *ls->v);
	if (!ls->v) {
		lineset_free(ls);
		return -1;
	}

	for (i = 0; i < len; i++) {
		if (ls->buf[i] != '\n')
			continue;
		ls->v[k++] = (struct lline){
			.boff = start,
			.len = (uint32_t)(i - start + 1),
			.hash = hash_line(ls->buf + start, i - start + 1),
		};
		start = i + 1;
	}
	if (start < len)
		ls->v[k++] = (struct lline){
			.boff = start,
			.len = (uint32_t)(len - start),
			.hash = hash_line(ls->buf + start, len - start),
		};

	ls->n = k;
	return 0;
}

static bool lline_eq(const struct lineset *a, size_t i,
		     const struct lineset *b, size_t j)
{
	const struct lline *x = &a->v[i], *y = &b->v[j];

	return x->len == y->len && x->hash == y->hash &&
	       memcmp(a->buf + x->boff, b->buf + y->boff, x->len) == 0;
}

void edits_free(struct editlist *el)
{
	free(el->v);
	memset(el, 0, sizeof *el);
}

/* Adjacent edits that touch on both sides are one edit. */
static int edit_push(struct editlist *el, size_t alo, size_t ahi,
		     size_t blo, size_t bhi)
{
	if (alo == ahi && blo == bhi)
		return 0;

	if (el->n) {
		struct edit *last = &el->v[el->n - 1];

		if (last->ahi == alo && last->bhi == blo) {
			last->ahi = ahi;
			last->bhi = bhi;
			return 0;
		}
	}

	if (el->n == el->cap) {
		size_t ncap = el->cap ? el->cap * 2 : 32;
		struct edit *nv = realloc(el->v, ncap * sizeof *nv);

		if (!nv)
			return -1;
		el->v = nv;
		el->cap = ncap;
	}

	el->v[el->n++] = (struct edit){alo, ahi, blo, bhi};
	return 0;
}

/*
 * Exact answer for a small range, by the usual longest-common-subsequence
 * table filled from the back.
 */
static int diff_small(const struct lineset *a, size_t alo, size_t ahi,
		      const struct lineset *b, size_t blo, size_t bhi,
		      struct editlist *out)
{
	size_t na = ahi - alo, nb = bhi - blo;
	size_t w = nb + 1;
	unsigned int *dp;
	size_t i, j;
	int rc = -1;

	dp = malloc((na + 1) * w * sizeof *dp);
	if (!dp)
		return -1;

	for (i = na + 1; i-- > 0;) {
		for (j = nb + 1; j-- > 0;) {
			if (i == na || j == nb) {
				dp[i * w + j] = 0;
			} else if (lline_eq(a, alo + i, b, blo + j)) {
				dp[i * w + j] = 1 + dp[(i + 1) * w + j + 1];
			} else {
				unsigned int x = dp[(i + 1) * w + j];
				unsigned int y = dp[i * w + j + 1];

				dp[i * w + j] = x > y ? x : y;
			}
		}
	}

	i = j = 0;
	while (i < na || j < nb) {
		size_t si, sj;

		if (i < na && j < nb && lline_eq(a, alo + i, b, blo + j)) {
			i++;
			j++;
			continue;
		}

		si = i;
		sj = j;
		while (i < na || j < nb) {
			if (i < na && j < nb &&
			    lline_eq(a, alo + i, b, blo + j))
				break;
			if (i < na && j < nb) {
				if (dp[(i + 1) * w + j] >= dp[i * w + j + 1])
					i++;
				else
					j++;
			} else if (i < na) {
				i++;
			} else {
				j++;
			}
		}

		if (edit_push(out, alo + si, alo + i, blo + sj, blo + j) < 0)
			goto out;
	}

	rc = 0;
out:
	free(dp);
	return rc;
}

struct hidx {
	uint64_t hash;
	size_t idx;
};

static int hidx_cmp(const void *pa, const void *pb)
{
	const struct hidx *a = pa, *b = pb;

	if (a->hash != b->hash)
		return a->hash < b->hash ? -1 : 1;
	return 0;
}

/*
 * Fill `v` with the indices of lines that occur exactly once in the range,
 * sorted by hash so the two sides can be intersected.
 */
static size_t unique_lines(const struct lineset *ls, size_t lo, size_t hi,
			   struct hidx *v)
{
	size_t n = 0, i = lo;

	while (i < hi) {
		size_t j = i;

		while (j < hi && ls->v[j].hash == ls->v[i].hash)
			j++;
		/* Equal hashes need not mean equal lines, but for choosing an
		 * anchor that is harmless: a wrong choice only costs an
		 * opportunity, and the bytes are compared either way. */
		if (j - i == 1) {
			v[n].hash = ls->v[i].hash;
			v[n].idx = i;
			n++;
		}
		i = j;
	}
	return n;
}

/*
 * Split a large range at a line that is unique on both sides, which is the
 * line most likely to be a real correspondence rather than a coincidence.
 */
static int diff_anchor(const struct lineset *a, size_t alo, size_t ahi,
		       const struct lineset *b, size_t blo, size_t bhi,
		       struct editlist *out, int depth);

static int diff_rec(const struct lineset *a, size_t alo, size_t ahi,
		    const struct lineset *b, size_t blo, size_t bhi,
		    struct editlist *out, int depth)
{
	/* Strip the common ends first: it is cheap, and it usually leaves
	 * very little behind. */
	while (alo < ahi && blo < bhi && lline_eq(a, alo, b, blo)) {
		alo++;
		blo++;
	}
	while (alo < ahi && blo < bhi && lline_eq(a, ahi - 1, b, bhi - 1)) {
		ahi--;
		bhi--;
	}

	if (alo == ahi && blo == bhi)
		return 0;
	if (alo == ahi || blo == bhi)
		return edit_push(out, alo, ahi, blo, bhi);

	if (ahi - alo <= SMALL && bhi - blo <= SMALL)
		return diff_small(a, alo, ahi, b, blo, bhi, out);

	if (depth <= 0)
		return edit_push(out, alo, ahi, blo, bhi);

	return diff_anchor(a, alo, ahi, b, blo, bhi, out, depth);
}

static int diff_anchor(const struct lineset *a, size_t alo, size_t ahi,
		       const struct lineset *b, size_t blo, size_t bhi,
		       struct editlist *out, int depth)
{
	struct hidx *ua, *ub;
	size_t na = 0, nb = 0;
	size_t ia, ib;
	size_t best_i = 0, best_j = 0;
	bool found = false;
	uint64_t best_dist = UINT64_MAX;
	uint64_t mid_a = alo + (ahi - alo) / 2;
	uint64_t mid_b = blo + (bhi - blo) / 2;
	int rc = -1;

	ua = malloc((ahi - alo) * sizeof *ua);
	ub = malloc((bhi - blo) * sizeof *ub);
	if (!ua || !ub)
		goto out;

	for (ia = alo; ia < ahi; ia++)
		ua[na++] = (struct hidx){a->v[ia].hash, ia};
	for (ib = blo; ib < bhi; ib++)
		ub[nb++] = (struct hidx){b->v[ib].hash, ib};
	qsort(ua, na, sizeof *ua, hidx_cmp);
	qsort(ub, nb, sizeof *ub, hidx_cmp);

	na = unique_lines(a, alo, ahi, ua);
	nb = unique_lines(b, blo, bhi, ub);

	/* Intersect the two unique sets, keeping the match nearest the
	 * middle so the recursion stays balanced. */
	ia = ib = 0;
	while (ia < na && ib < nb) {
		if (ua[ia].hash < ub[ib].hash) {
			ia++;
		} else if (ua[ia].hash > ub[ib].hash) {
			ib++;
		} else {
			uint64_t d = (ua[ia].idx > mid_a ? ua[ia].idx - mid_a
							 : mid_a - ua[ia].idx) +
				     (ub[ib].idx > mid_b ? ub[ib].idx - mid_b
							 : mid_b - ub[ib].idx);

			if (!found || d < best_dist) {
				best_dist = d;
				best_i = ua[ia].idx;
				best_j = ub[ib].idx;
				found = true;
			}
			ia++;
			ib++;
		}
	}

	if (!found) {
		/* Nothing unique to split on.  Reporting the range as wholly
		 * replaced is coarse but honest; a finer answer would need a
		 * full search, which is exactly what this tool exists to
		 * avoid. */
		rc = edit_push(out, alo, ahi, blo, bhi);
		goto out;
	}

	if (diff_rec(a, alo, best_i, b, blo, best_j, out, depth - 1) < 0)
		goto out;
	if (diff_rec(a, best_i + 1, ahi, b, best_j + 1, bhi, out, depth - 1) < 0)
		goto out;

	rc = 0;
out:
	free(ua);
	free(ub);
	return rc;
}

int linediff(const struct lineset *a, const struct lineset *b,
	     struct editlist *out)
{
	out->v = NULL;
	out->n = 0;
	out->cap = 0;

	/* Depth cap keeps the recursion bounded on pathological input; each
	 * level at least splits the range, so this is generous. */
	return diff_rec(a, 0, a->n, b, 0, b->n, out, 64);
}

/* ---- line boundary helpers -------------------------------------------- */

int line_start(int fd, uint64_t off, uint64_t *out)
{
	unsigned char buf[4096];

	while (off > 0) {
		uint64_t n = off < sizeof buf ? off : sizeof buf;
		uint64_t base = off - n;
		ssize_t i;

		if (pread_full(fd, buf, n, base) < 0)
			return -1;
		for (i = (ssize_t)n - 1; i >= 0; i--) {
			if (buf[i] == '\n') {
				*out = base + (uint64_t)i + 1;
				return 0;
			}
		}
		off = base;
	}
	*out = 0;
	return 0;
}

int line_end(int fd, uint64_t off, uint64_t limit, uint64_t *out)
{
	unsigned char buf[4096];

	while (off < limit) {
		uint64_t n = limit - off < sizeof buf ? limit - off : sizeof buf;
		ssize_t i;

		if (pread_full(fd, buf, n, off) < 0)
			return -1;
		for (i = 0; i < (ssize_t)n; i++) {
			if (buf[i] == '\n') {
				*out = off + (uint64_t)i + 1;
				return 0;
			}
		}
		off += n;
	}
	*out = limit;
	return 0;
}
