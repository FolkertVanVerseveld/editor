#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#define BF_READONLY 1
#define BF_ROWMAX 16

static const char *help_editor =
	"Line-based hex editor\n"
	"Commands:\n"
	"g                   Show file info\n"
	"m start [length]    Dump memory\n"
	"q                   Quit editor";

static const char *help_showinfo =
	"g: g\n"
	"  Print file name and size in hexadecimal.";

static const char *help_peek =
	"m: m start [length]\n"
	"  Dump memory from START to START + LENGTH. If length is not specified,\n"
	"  a default value is implied.";

static const char *help_quit =
	"q: q\n"
	"  Quit editor without saving changes.";

/* binary file */
static struct bfile {
	int fd;
	mode_t mode;
	unsigned flags;
	void *data;
	size_t size;
	struct stat st;
	const char *name;
} file;

static int bfile_open(struct bfile *f, const char *name, mode_t mode)
{
	int fd = -1, prot = PROT_READ;
	unsigned flags = 0;
	void *data;
	size_t size;
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
	if (!(flags & BF_READONLY))
		prot |= PROT_WRITE;
	data = mmap(NULL, size = f->st.st_size, prot, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "Can't map %s: %s\n", name, strerror(errno));
		return 1;
	}
	f->fd = fd;
	f->flags = flags;
	f->mode = mode;
	f->name = name;
	f->data = data;
	f->size = size;
	return 0;
}

static void bfile_close(struct bfile *f)
{
	if (f->data != MAP_FAILED) {
		munmap(f->data, f->size);
		f->data = MAP_FAILED;
	}
	if (f->fd != -1) {
		close(f->fd);
		f->fd = -1;
	}
}

static void bfile_showinfo(const struct bfile *f)
{
	printf("%s, size: %zX\n", f->name, f->st.st_size);
}

static void bfile_peek(const struct bfile *f, uint64_t start, unsigned length)
{
	unsigned row_counter = 0;
	const uint8_t *map = f->data;
	for (; length && start < f->size; ++start, --length) {
		printf(" %02" PRIX8, map[start]);
		if (++row_counter == BF_ROWMAX) {
			row_counter = 0;
			putchar('\n');
		}
	}
	for (; length; ++start, --length) {
		fputs(" ~~", stdout);
		if (++row_counter == BF_ROWMAX) {
			row_counter = 0;
			putchar('\n');
		}
	}
	if (row_counter)
		putchar('\n');
}

static int parse_address(const char *str, uint64_t *address, uint64_t *mask)
{
	const char *base_str = "0123456789ABCDEF";
	unsigned base = 10;
	if (!*str)
		return 1;
	const char *ptr = str;
	uint64_t v = 0;
	switch (*ptr) {
	case '%': base =  2; ++ptr; break;
	case 'o': base =  8; ++ptr; break;
	case '$': base = 16; ++ptr; break;
	}
	*mask = 0;
	for (; *ptr; ++ptr) {
		v *= base;
		*mask *= base;
		const char *pos = strchr(base_str, toupper(*ptr));
		if (!pos)
			return 1;
		v += (unsigned)(pos - base_str);
		*mask += base - 1;
	}
	*address = v;
	return 0;
}

static int help(const char *start)
{
	switch (*start) {
	case '\0':
		puts(help_editor);
		break;
	case 'g':
		puts(help_showinfo);
		break;
	case 'm':
		puts(help_peek);
		break;
	case 'q':
		puts(help_quit);
		break;
	default:
		return -1;
	}
	return 0;
}

static int peek(char *start)
{
	const char *delim = " \f\n\r\t\v";
	char *token, *saveptr, *str;
	unsigned n = 0;
	uint64_t addr, length = 16, mask;
	for (str = start; ; str = NULL) {
		token = strtok_r(str, delim, &saveptr);
		if (!token)
			break;
		switch (n++) {
		case 0:
			if (parse_address(token, &addr, &mask)) {
				fputs("Bad start address\n", stderr);
				return 1;
			}
			break;
		case 1:
			if (parse_address(token, &length, &mask)) {
				fputs("Bad length\n", stderr);
				return 1;
			}
			if (!length || length > 256) {
				fputs("Length not in range [1,256]\n", stderr);
				return 1;
			}
			break;
		default:
			fprintf(stderr, "Unexpected garbage: %s\n", token);
			return 1;
		}
	}
	if (!n) {
		fputs("Missing start address\n", stderr);
		return 1;
	}
	bfile_peek(&file, addr, length);
	return 0;
}

static int parse(char *start)
{
	char *op;
	for (op = start + 1; isspace(*op); ++op)
		;
	switch (*start) {
	case '?':
		return help(op);
	case 'g':
		bfile_showinfo(&file);
		break;
	case 'm':
		return peek(op);
	default:
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
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
		if (parse(start) < 0)
			fprintf(stderr, "? %s\n", start);
	}
	bfile_close(&file);
	return 0;
}
