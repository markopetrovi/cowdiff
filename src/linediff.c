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

/*
 * Which lines occur exactly once in a file.
 *
 * The anchor search needs this at every level of its recursion, and the
 * obvious way to ask -- sort the range and look for runs of one -- costs a
 * sort per level, which is where nearly all of this tool's time used to go.
 * Counting hashes once, up front, answers it for every level at once: a line
 * that occurs once in the whole file occurs once in any part of it.
 *
 * A hash is not the line, so a caller that acts on a match still has to
 * compare the text.  Treating two colliding lines as one is not merely
 * imprecise here -- it would mean skipping both from the diff and quietly
 * dropping a real difference -- so the anchor search verifies every match.
 */
struct hent {
	uint64_t hash;
	uint32_t idx;		/* first (only) index, when count is 1 */
	uint32_t count;		/* zero means the slot is empty */
};

struct htab {
	struct hent *v;
	size_t mask;
};

/* The tables, built on first need rather than up front.  A diff that the
 * common-prefix and common-suffix trimming resolves never reaches the anchor
 * search, and making it pay for a pass over the whole file first is a tax on
 * exactly the easy case. */
struct dstate {
	struct htab ta, tb;
	bool built;
};

static int htab_init(struct htab *t, size_t n)
{
	size_t cap = 16;

	while (cap < n * 2)
		cap <<= 1;
	t->v = calloc(cap, sizeof *t->v);
	if (!t->v)
		return -1;
	t->mask = cap - 1;
	return 0;
}

static struct hent *htab_slot(const struct htab *t, uint64_t hash)
{
	size_t i = (size_t)hash & t->mask;

	while (t->v[i].count && t->v[i].hash != hash)
		i = (i + 1) & t->mask;
	return (struct hent *)&t->v[i];
}

static int htab_count(const struct lineset *ls, struct htab *t)
{
	size_t i;

	if (htab_init(t, ls->n) < 0)
		return -1;
	for (i = 0; i < ls->n; i++) {
		struct hent *e = htab_slot(t, ls->v[i].hash);

		if (!e->count) {
			e->hash = ls->v[i].hash;
			e->idx = (uint32_t)i;
			e->count = 1;
		} else {
			e->count++;
		}
	}
	return 0;
}

static const struct hent *htab_find(const struct htab *t, uint64_t hash)
{
	const struct hent *e = htab_slot(t, hash);

	return e->count ? e : NULL;
}

static int dstate_build(struct dstate *ds, const struct lineset *a,
			const struct lineset *b)
{
	if (ds->built)
		return ds->ta.v && ds->tb.v ? 0 : -1;

	ds->built = true;
	if (htab_count(a, &ds->ta) < 0)
		return -1;
	return htab_count(b, &ds->tb);
}

/*
 * Split a large range at a line that is unique on both sides, which is the
 * line most likely to be a real correspondence rather than a coincidence.
 */
static int diff_anchor(const struct lineset *a, size_t alo, size_t ahi,
		       const struct lineset *b, size_t blo, size_t bhi,
		       struct editlist *out, int depth, struct dstate *ds);

static int diff_rec(const struct lineset *a, size_t alo, size_t ahi,
		    const struct lineset *b, size_t blo, size_t bhi,
		    struct editlist *out, int depth, struct dstate *ds)
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

	return diff_anchor(a, alo, ahi, b, blo, bhi, out, depth, ds);
}

static int diff_anchor(const struct lineset *a, size_t alo, size_t ahi,
		       const struct lineset *b, size_t blo, size_t bhi,
		       struct editlist *out, int depth, struct dstate *ds)
{
	size_t i, best_i = 0, best_j = 0;
	bool found = false;
	uint64_t best_dist = UINT64_MAX;
	uint64_t mid_a = alo + (ahi - alo) / 2;
	uint64_t mid_b = blo + (bhi - blo) / 2;

	if (dstate_build(ds, a, b) < 0)
		return -1;

	/*
	 * Walk this side for a line that occurs exactly once in each file --
	 * the line most likely to be a real correspondence rather than a
	 * coincidence between two common ones -- and take the match nearest
	 * the middle so the recursion stays balanced.
	 */
	for (i = alo; i < ahi; i++) {
		const struct hent *ea = htab_find(&ds->ta, a->v[i].hash);
		const struct hent *eb;
		uint64_t d;

		if (!ea || ea->count != 1 || ea->idx != i)
			continue;
		eb = htab_find(&ds->tb, a->v[i].hash);
		if (!eb || eb->count != 1)
			continue;
		if (eb->idx < blo || eb->idx >= bhi)
			continue;
		/*
		 * The tables hold hashes and equal hashes are not equal
		 * lines.  Splitting on a collision would skip both lines
		 * from the diff and drop a real difference, so confirm the
		 * text before relying on it.
		 */
		if (!lline_eq(a, i, b, eb->idx))
			continue;

		d = (i > mid_a ? i - mid_a : mid_a - i) +
		    (eb->idx > mid_b ? eb->idx - mid_b : mid_b - eb->idx);
		if (!found || d < best_dist) {
			best_dist = d;
			best_i = i;
			best_j = eb->idx;
			found = true;
		}
	}

	if (!found) {
		/* Nothing unique to split on.  Reporting the range as wholly
		 * replaced is coarse but honest; a finer answer would need a
		 * full search, which is exactly what this tool exists to
		 * avoid. */
		return edit_push(out, alo, ahi, blo, bhi);
	}

	if (diff_rec(a, alo, best_i, b, blo, best_j, out, depth - 1, ds) < 0)
		return -1;
	if (diff_rec(a, best_i + 1, ahi, b, best_j + 1, bhi, out, depth - 1,
		     ds) < 0)
		return -1;
	return 0;
}

int linediff(const struct lineset *a, const struct lineset *b,
	     struct editlist *out)
{
	struct dstate ds;
	int rc;

	out->v = NULL;
	out->n = 0;
	out->cap = 0;

	memset(&ds, 0, sizeof ds);

	/* Depth cap keeps the recursion bounded on pathological input; each
	 * level at least splits the range, so this is generous. */
	rc = diff_rec(a, 0, a->n, b, 0, b->n, out, 64, &ds);

	free(ds.ta.v);
	free(ds.tb.v);
	return rc;
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
