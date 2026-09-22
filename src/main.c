/*
 * cowdiff - diff two files using the filesystem's extent map.
 */
#define _GNU_SOURCE
#include "cowdiff.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/fiemap.h>

static bool opt_brief, opt_stats, opt_force_binary, opt_force_text;
static bool opt_byte_offsets, opt_recursive, opt_dump;

static void usage(FILE *f, const char *argv0)
{
	fprintf(f,
"usage: %s [options] FILE1 FILE2\n"
"\n"
"Compare two files using the filesystem's own extent map, so that data which\n"
"is physically shared -- reflinks, snapshots, deduplicated blocks -- is never\n"
"read at all.  Holes and unwritten extents are skipped rather than read as\n"
"zeros, and only ranges that are not shared get read and compared.\n"
"\n"
"options:\n"
"  -q, --brief           report only whether the files differ\n"
"      --stats           report how many bytes were actually read\n"
"      --byte-offsets    put byte offsets in hunk headers instead of line\n"
"                        numbers; skips the scan that line numbers need, at\n"
"                        the cost of output that patch(1) cannot consume\n"
"  -r, --recursive       compare directories recursively\n"
"  -a, --text            treat the files as text even if they look binary\n"
"      --force-binary    report byte ranges even if they look like text\n"
"  -U NUM                lines of context around each change (default 3)\n"
"  -u, --unified         accepted and ignored; unified is the only format\n"
"      --dump-extents    print the extent map and exit; with one FILE this\n"
"                        is that file's map, with two it is both\n"
"  -h, --help            this message\n"
"\n"
"exit status: 0 files are identical, 1 they differ, 2 an error occurred\n",
		argv0);
}

/*
 * Context lines, from -U NUM, --unified=NUM, or a bundle ending in U.
 *
 * Strict about the value, as diff is: "-Uabc" is a mistake worth reporting
 * rather than a silent zero, and a context of one line where ten were meant
 * is the kind of difference a person does not notice in the output.
 */
static int set_context(const char *v)
{
	char *end;
	long n;

	errno = 0;
	n = strtol(v, &end, 10);
	if (end == v || *end != '\0' || errno == ERANGE || n < 0 || n > 1000000) {
		fprintf(stderr, "cowdiff: invalid context length '%s'\n", v);
		return -1;
	}
	text_diff_set_context((int)n);
	return 0;
}

static void dump_map(const char *path, const struct extmap *m)
{
	size_t i;

	printf("%s: %zu extents, %llu bytes\n", path, m->n,
	       (unsigned long long)m->size);
	for (i = 0; i < m->n; i++) {
		const struct ext *e = &m->v[i];

		printf("  off=%-12llu len=%-10llu phys=%-14llu %s%s%s%s\n",
		       (unsigned long long)e->off, (unsigned long long)e->len,
		       (unsigned long long)e->phys,
		       e->trusted ? "trusted" : "untrusted",
		       (e->flags & FIEMAP_EXTENT_ENCODED) ? " encoded" : "",
		       (e->flags & FIEMAP_EXTENT_UNWRITTEN) ? " unwritten" : "",
		       (e->flags & FIEMAP_EXTENT_SHARED) ? " shared" : "");
	}
}

/*
 * Open a file and read its extent map.  Both the comparison and
 * --dump-extents need exactly this, so neither owns a copy of it.
 */
static int open_map(const char *path, struct extmap *m, int *fd_out,
		    struct stat *st)
{
	int fd;

	extmap_init(m);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "cowdiff: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fstat(fd, st) < 0) {
		fprintf(stderr, "cowdiff: %s: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (S_ISDIR(st->st_mode)) {
		fprintf(stderr, "cowdiff: %s: is a directory (use -r)\n", path);
		close(fd);
		return -1;
	}
	if (extmap_load(m, fd, (uint64_t)st->st_size) < 0) {
		fprintf(stderr, "cowdiff: %s: cannot read extent map: %s\n",
			path, strerror(errno));
		close(fd);
		return -1;
	}

	*fd_out = fd;
	return 0;
}

/* --dump-extents on a single file: no comparison, just the map. */
static int dump_one(const char *path)
{
	struct extmap m;
	struct stat st;
	int fd;

	if (open_map(path, &m, &fd, &st) < 0)
		return 2;

	dump_map(path, &m);
	close(fd);
	extmap_free(&m);
	return 0;
}

int compare_files(const char *pa, const char *pb, bool in_recursion)
{
	struct extmap ma, mb;
	struct anchorlist al;
	struct deltalist dl;
	struct stat sa, sb;
	bool cmp_addr;
	int fd_a = -1, fd_b = -1;
	int rc = 2, out;

	extmap_init(&ma);
	extmap_init(&mb);
	memset(&al, 0, sizeof al);
	memset(&dl, 0, sizeof dl);

	/*
	 * --stats counts what this file cost, not what the run has cost: with
	 * -r every file is reported separately, and a running total would make
	 * the percentage of the bytes read climb past 100.
	 */
	cowdiff_bytes_read = 0;

	if (open_map(pa, &ma, &fd_a, &sa) < 0 ||
	    open_map(pb, &mb, &fd_b, &sb) < 0)
		goto out;

	/*
	 * --dump-extents asks for the map, not for a comparison, so it comes
	 * before the shortcut below.  Naming the same file twice is a fair way
	 * to ask for one map twice, and it used to print "identical" instead.
	 */
	if (opt_dump) {
		dump_map(pa, &ma);
		dump_map(pb, &mb);
		rc = 0;
		goto out;
	}

	/* The same inode twice needs no work and no reads. */
	if (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) {
		/* Neither -q nor -r says anything about files that match: -q
		 * because the status is the whole answer, -r because it reports
		 * only the files that differ.  This path is the one that can
		 * reach "identical" without comparing, so it is also the one
		 * that has to check both. */
		if (!in_recursion && !opt_brief)
			printf("Files %s and %s are identical\n", pa, pb);
		rc = 0;
		/* Through stats, not past it: --stats describes what the
		 * comparison read, and this one read nothing. */
		goto stats;
	}

	/*
	 * Addresses only mean something within one filesystem.  Across two
	 * of them, equal numbers are a coincidence, and trusting them would
	 * mean calling different files identical.
	 */
	cmp_addr = same_filesystem(fd_a, fd_b);

	if (anchors_find(&ma, &mb, cmp_addr, &al) < 0) {
		fprintf(stderr, "cowdiff: out of memory\n");
		goto out;
	}

	/*
	 * A chain is a correct alignment for the gaps it leaves, but a chain of
	 * matches that sit at *different* file offsets is not evidence that the
	 * two files are equal: the address proves the content is equal, not
	 * that those bytes agree where they sit.  With the shares crossed -- A's
	 * block at one offset shared with B's at another, and vice versa -- the
	 * only chains available are shifted ones, the gaps they leave are
	 * one-sided, and deltas_find's insertion/deletion branches report those
	 * as differences without reading a byte, so two identical files came
	 * out "different".  Dropping the shifted matches leaves a chain in which
	 * every gap is at the same offsets on both sides (by induction from
	 * offset 0: the anchors keep prev_a == prev_b), so every gap is compared
	 * and those branches cannot be reached at all.
	 *
	 * Only when the lengths match.  Where they differ the verdict costs
	 * nothing to reach, and a shifted match is what saves reading the tail
	 * of a file that had bytes inserted before it.
	 */
	if (sa.st_size == sb.st_size)
		anchors_keep_same_offset(&al);
	if (deltas_find(fd_a, &ma, (uint64_t)sa.st_size, fd_b, &mb,
			(uint64_t)sb.st_size, &al, &dl, opt_brief) < 0) {
		fprintf(stderr, "cowdiff: %s\n", strerror(errno));
		goto out;
	}

	if (dl.n == 0) {
		/* Neither diff -r nor diff -q says anything about files that
		 * match. */
		if (!in_recursion && !opt_brief)
			printf("Files %s and %s are identical\n", pa, pb);
		rc = 0;
		goto stats;
	}

	if (opt_brief) {
		printf("Files %s and %s differ\n", pa, pb);
		rc = 1;
		goto stats;
	}

	/* diff -r names each file before reporting on it.  We only have one
	 * output format, so the line always says -ru. */
	if (in_recursion)
		printf("diff -ru %s %s\n", pa, pb);

	if (opt_force_binary || (!opt_force_text && (is_binary(fd_a) ||
						     is_binary(fd_b)))) {
		out = emit_binary_diff(pa, pb, &dl);
	} else {
		out = emit_text_diff(pa, pb, fd_a, fd_b, &ma, &mb, &dl,
				     opt_byte_offsets);
	}
	if (out < 0) {
		fprintf(stderr, "cowdiff: %s\n", strerror(errno));
		goto out;
	}
	rc = out ? 1 : 0;

stats:
	/*
	 * After the output, not before: text mode reads more than the
	 * comparison does, because a hunk header carries line numbers and a
	 * line number costs a pass over the file.  Reporting first would hide
	 * exactly the cost worth knowing about.
	 */
	if (opt_stats) {
		unsigned long long total = (unsigned long long)sa.st_size +
					   (unsigned long long)sb.st_size;
		unsigned long long covered = 0;
		size_t k;

		for (k = 0; k < al.n; k++)
			covered += al.v[k].len;

		fprintf(stderr,
			"cowdiff: %zu anchor%s proving %llu of %llu bytes equal (%.4f%%)\n"
			"cowdiff: read %llu of %llu bytes (%.4f%%)\n",
			al.n, al.n == 1 ? "" : "s", covered, total,
			total ? 100.0 * (double)covered / (double)total : 0.0,
			(unsigned long long)cowdiff_bytes_read, total,
			total ? 100.0 * (double)cowdiff_bytes_read / (double)total
			      : 0.0);
	}

out:
	anchors_free(&al);
	deltas_free(&dl);
	extmap_free(&ma);
	extmap_free(&mb);
	if (fd_a >= 0)
		close(fd_a);
	if (fd_b >= 0)
		close(fd_b);
	return rc;
}

int main(int argc, char **argv)
{
	const char *pa = NULL, *pb = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] == '-' && a[1] == '-' && a[2] != '\0') {
			if (!strcmp(a, "--help")) {
				usage(stdout, argv[0]);
				return 0;
			} else if (!strcmp(a, "--brief")) {
				opt_brief = true;
			} else if (!strcmp(a, "--stats")) {
				opt_stats = true;
			} else if (!strcmp(a, "--force-binary")) {
				opt_force_binary = true;
			} else if (!strcmp(a, "--byte-offsets")) {
				opt_byte_offsets = true;
			} else if (!strcmp(a, "--text")) {
				opt_force_text = true;
			} else if (!strcmp(a, "--recursive")) {
				opt_recursive = true;
			} else if (!strcmp(a, "--dump-extents")) {
				opt_dump = true;
			} else if (!strcmp(a, "--unified")) {
				/*
				 * Unified is the only format there is, so
				 * this asks for what already happens.
				 * Accepted rather than refused: it is the
				 * flag most often passed to diff out of
				 * habit, and failing on it would be a poor
				 * way to be a drop-in.
				 */
			} else if (!strncmp(a, "--unified=", 10)) {
				if (set_context(a + 10) < 0)
					return 2;
			} else {
				fprintf(stderr, "cowdiff: unknown option %s\n", a);
				usage(stderr, argv[0]);
				return 2;
			}
			continue;
		}

		if (a[0] == '-' && a[1] != '\0') {
			/*
			 * Short options, bunched the way diff accepts them --
			 * "-rq" is how scripts write "-r -q", and refusing it
			 * would fail on the invocation rather than the
			 * comparison.  U takes a value: the rest of this
			 * argument if there is any, the next one otherwise.
			 */
			const char *p = a + 1;

			while (*p) {
				const char *v;

				if (*p != 'U') {
					switch (*p) {
					case 'q':
						opt_brief = true;
						break;
					case 'r':
						opt_recursive = true;
						break;
					case 'a':
						opt_force_text = true;
						break;
					case 'u':
						break;	/* the only format */
					case 'h':
						usage(stdout, argv[0]);
						return 0;
					default:
						fprintf(stderr,
							"cowdiff: unknown option -%c\n",
							*p);
						usage(stderr, argv[0]);
						return 2;
					}
					p++;
					continue;
				}

				v = p + 1;
				if (*v == '\0') {
					if (++i >= argc) {
						fprintf(stderr,
							"cowdiff: -U needs a number\n");
						return 2;
					}
					v = argv[i];
				}
				if (set_context(v) < 0)
					return 2;
				break;		/* the value ended the bunch */
			}
			continue;
		}

		if (!pa) {
			pa = a;
		} else if (!pb) {
			pb = a;
		} else {
			fprintf(stderr, "cowdiff: too many arguments\n");
			usage(stderr, argv[0]);
			return 2;
		}
	}

	/* Dumping a map needs one file; comparing needs two. */
	if (!pa || (!pb && !opt_dump)) {
		usage(stderr, argv[0]);
		return 2;
	}

	if (opt_dump && !pb)
		return dump_one(pa);

	if (opt_recursive) {
		struct stat sa, sb;

		/* -r on two plain files is just a comparison; diff agrees. */
		if (stat(pa, &sa) == 0 && stat(pb, &sb) == 0 &&
		    S_ISREG(sa.st_mode) && S_ISREG(sb.st_mode))
			return compare_files(pa, pb, false);
		return walk_trees(pa, pb);
	}

	return compare_files(pa, pb, false);
}
