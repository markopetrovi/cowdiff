/*
 * walk.c - compare two directory trees.
 *
 * The traversal deliberately mirrors diff -r: entries are visited in sorted
 * order, a name present on only one side is reported and skipped, and a pair
 * of regular files is handed to the ordinary comparison, which prints the
 * "diff -ru A B" line diff prints before each file that differs.
 *
 * Symlinks are followed, as diff does by default, so a broken one is an error
 * against the file it points at rather than a difference between two links.
 */
#define _GNU_SOURCE
#include "cowdiff.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* 0 nothing differed, 1 something did, 2 something went wrong. */
static int status;

static void note(int s)
{
	if (s > status)
		status = s;
}

static int name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void free_list(char **v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

/*
 * Names in a directory, sorted the way diff orders them.  Returns NULL with
 * *n_out zero on failure, having already complained.
 */
static char **list_dir(const char *path, size_t *n_out)
{
	DIR *d;
	struct dirent *e;
	char **v = NULL;
	size_t n = 0, cap = 0;

	*n_out = 0;
	d = opendir(path);
	if (!d) {
		fprintf(stderr, "cowdiff: %s: %s\n", path, strerror(errno));
		note(2);
		return NULL;
	}

	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (n == cap) {
			size_t ncap = cap ? cap * 2 : 64;
			char **nv = realloc(v, ncap * sizeof *nv);

			if (!nv) {
				free_list(v, n);
				closedir(d);
				note(2);
				*n_out = 0;
				return NULL;
			}
			v = nv;
			cap = ncap;
		}
		v[n] = strdup(e->d_name);
		if (!v[n]) {
			free_list(v, n);
			closedir(d);
			note(2);
			*n_out = 0;
			return NULL;
		}
		n++;
	}
	closedir(d);

	if (n > 1)
		qsort(v, n, sizeof *v, name_cmp);
	*n_out = n;
	return v;
}

static char *join(const char *dir, const char *name)
{
	size_t dl = strlen(dir), nl = strlen(name);
	bool slash = dl > 0 && dir[dl - 1] != '/';
	char *p = malloc(dl + nl + 2);

	if (!p)
		return NULL;
	memcpy(p, dir, dl);
	if (slash)
		p[dl++] = '/';
	memcpy(p + dl, name, nl + 1);
	return p;
}

/*
 * The directories being walked, so that a loop -- a symlink pointing at a
 * directory above it, which stat() happily follows -- is reported rather than
 * followed until the kernel gives up at forty levels of it.
 *
 * One node per level holds both sides of the pair, since either may be the
 * one that loops, and the chain lives on the C stack: a level is exactly as
 * long as the recursion that made it.
 */
struct level {
	const struct level *up;
	bool have_a, have_b;	/* that side was identified */
	dev_t dev_a, dev_b;
	ino_t ino_a, ino_b;
};

/* Is either directory one we are already inside?  A side that could not be
 * identified is not compared: dev and ino of zero are real values on some
 * filesystems, so "unknown" has to be tracked, not encoded. */
static bool is_ancestor(const struct level *up, const struct stat *sa,
			const struct stat *sb)
{
	for (; up; up = up->up) {
		if (up->have_a && up->dev_a == sa->st_dev &&
		    up->ino_a == sa->st_ino)
			return true;
		if (up->have_b && up->dev_b == sb->st_dev &&
		    up->ino_b == sb->st_ino)
			return true;
	}
	return false;
}

static const char *typename_of(mode_t m)
{
	if (S_ISDIR(m))
		return "directory";
	if (S_ISREG(m))
		return "regular file";
	if (S_ISFIFO(m))
		return "fifo";
	if (S_ISLNK(m))
		return "symbolic link";
	if (S_ISCHR(m))
		return "character special file";
	if (S_ISBLK(m))
		return "block special file";
	if (S_ISSOCK(m))
		return "socket";
	return "unknown file type";
}

static void walk_dir(const char *pa, const char *pb, const struct level *up)
{
	struct level here;
	struct stat sa, sb;
	size_t na = 0, nb = 0, i = 0, j = 0;
	char **la = list_dir(pa, &na);
	char **lb = list_dir(pb, &nb);

	/*
	 * Whatever is walked below this level is inside it, so this level
	 * becomes an ancestor of it.  Each side is recorded on its own: a
	 * stat() that fails takes that side's identity away and nothing else.
	 * The walk only recurses into a pair whose two sides were both stat'd
	 * by the time this is called, so the chain is complete on the path that
	 * matters -- and a level dropped whole would be a hole in the loop
	 * detection that nothing would notice until a symlink found it.
	 */
	here.up = up;
	here.have_a = stat(pa, &sa) == 0;
	here.have_b = stat(pb, &sb) == 0;
	if (here.have_a) {
		here.dev_a = sa.st_dev;
		here.ino_a = sa.st_ino;
	}
	if (here.have_b) {
		here.dev_b = sb.st_dev;
		here.ino_b = sb.st_ino;
	}
	up = &here;

	/*
	 * A listing that failed leaves nothing to pair against.  list_dir() has
	 * already said what went wrong and set the status, and that is all
	 * there is to say: every name the other side holds would otherwise come
	 * out as "Only in <dir>", which is a claim about *this* directory that
	 * was never established -- it may well hold all of them.
	 */
	if (!la || !lb) {
		free_list(la, na);
		free_list(lb, nb);
		return;
	}

	while (i < na || j < nb) {
		int c;

		if (i == na)
			c = 1;
		else if (j == nb)
			c = -1;
		else
			c = strcmp(la[i], lb[j]);

		if (c < 0) {
			printf("Only in %s: %s\n", pa, la[i]);
			note(1);
			i++;
			continue;
		}
		if (c > 0) {
			printf("Only in %s: %s\n", pb, lb[j]);
			note(1);
			j++;
			continue;
		}

		{
			char *fa = join(pa, la[i]);
			char *fb = join(pb, lb[j]);
			struct stat sa, sb;

			if (!fa || !fb) {
				note(2);
			} else if (stat(fa, &sa) < 0 || stat(fb, &sb) < 0) {
				/* Follow, so this is the broken one. */
				fprintf(stderr, "cowdiff: %s: %s\n",
					stat(fa, &sa) < 0 ? fa : fb,
					strerror(errno));
				note(2);
			} else if (S_ISDIR(sa.st_mode) && S_ISDIR(sb.st_mode)) {
				if (is_ancestor(up, &sa, &sb)) {
					/* diff(1) reports the same way and
					 * gives up on the pair rather than
					 * descending into it. */
					fprintf(stderr,
						"cowdiff: %s: recursive directory loop\n",
						fa);
					note(2);
				} else {
					walk_dir(fa, fb, up);
				}
			} else if (S_ISREG(sa.st_mode) && S_ISREG(sb.st_mode)) {
				note(compare_files(fa, fb, true));
			} else {
				printf("File %s is a %s while file %s is a %s\n",
				       fa, typename_of(sa.st_mode), fb,
				       typename_of(sb.st_mode));
				note(1);
			}
			free(fa);
			free(fb);
		}

		i++;
		j++;
	}

	free_list(la, na);
	free_list(lb, nb);
}

int walk_trees(const char *pa, const char *pb)
{
	status = 0;
	walk_dir(pa, pb, NULL);
	return status;
}
