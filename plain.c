#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#define BF_READONLY 1

/* binary file */
struct bfile {
	int fd;
	mode_t mode;
	unsigned flags;
	struct stat st;
};

static int bfile_open(struct bfile *f, const char *name, mode_t mode)
{
	int fd = -1;
	unsigned flags = 0;
	fd = open(name, O_RDWR | O_CREAT, mode);
	if (fd == -1) {
		fd = open(name, O_RDONLY, mode);
		if (fd == -1) {
			fprintf(stderr, "Can't open %s: %s\n", name, strerror(errno));
			return 1;
		}
		flags |= BF_READONLY;
	}
	if (fstat(fd, &f->st)) {
		fprintf(stderr, "Can't access %s: %s\n", name, strerror(errno));
		return 1;
	}
	f->fd = fd;
	f->flags = flags;
	f->mode = mode;
	return 0;
}

static void bfile_close(struct bfile *f)
{
	if (f->fd != -1) {
		close(f->fd);
		f->fd = -1;
	}
}

static int parse(const char *start, const char *end)
{
	size_t cmdlen = (size_t)(end - start);
	printf("exec \"%s\" (%zu)\n", start, cmdlen);
	return 0;
}

int main(int argc, char **argv)
{
	struct bfile file;
	int ret;
	char input[80];
	if (argc != 2) {
		fprintf(stderr, "usage: %s file\n", argc > 1 ? argv[0] : "editor");
		return 1;
	}
	ret = bfile_open(&file, argv[1], 0664);
	if (ret)
		return ret;
	while (fgets(input, sizeof input, stdin)) {
		char *start, *last, *end;
		/* trim input before processing ignoring all whitespace */
		for (start = input; isspace(*start); ++start)
			;
		for (end = start; *end; ++end)
			;
		for (last = end; last > start; --last)
			if (!isspace(last[-1]))
				break;
		*last = '\0';
		if (*start == 'q' && !start[1])
			break;
		if (parse(start, last) < 0)
			fprintf(stderr, "? %s\n", start);
	}
	bfile_close(&file);
	return 0;
}
