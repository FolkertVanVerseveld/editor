#define _GNU_SOURCE
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#define BF_READONLY 1
#define BF_ROWMAX 16

#define SPACE_DELIM " \f\n\r\t\v"
#define INPUT_BUFSZ 256

static const char *help_editor =
	"Line-based editor\n"
	"Commands:\n"
	"a start data...           Poke data\n"
	"c dest src length         Copy data\n"
	"g                         Show file info\n"
	"h start[,length] data...  Hunt data\n"
	"m start [length]          Dump memory\n"
	"q                         Quit editor\n"
	"t size                    Truncate file";

static const char *help_poke =
	"a: start data...\n"
	"  Poke data to START one number at a time. The space occupied by each\n"
	"  number is round up to a power of two times 8. Example:\n"
	"\n"
	"    a 0 $ff %110011001 o400\n"
	"\n"
	"  These numbers take up 1, 2 and 2 bytes respectively (0 is the start\n"
	"  address and therefore not included).";

static const char *help_copy =
	"c: c dest src length\n"
	"  Copy data from SRC to DEST. The areas may overlap.";

static const char *help_showinfo =
	"g: g\n"
	"  Print file name and size in hexadecimal.";

static const char *help_hunt =
	"h: h start[,length] data...\n"
	"  Find the first occurrence from START to START + LENGTH. If length is not\n"
	"  specified, it will search till the end of file.\n"
	"\n"
	"  Nothing is returned if DATA could not be found.";

static const char *help_peek =
	"m: m start [length]\n"
	"  Dump memory from START to START + LENGTH. If length is not specified,\n"
	"  a default value is implied.";

static const char *help_quit =
	"q: q\n"
	"  Quit editor and save changes.";

static const char *help_truncate =
	"t: t size\n"
	"  Resize file to SIZE and zero new data if resized file is bigger.";

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

static int bfile_truncate(struct bfile *f, uint64_t size)
{
	if (file.flags & BF_READONLY) {
		fputs("Can't truncate: readonly file\n", stderr);
		return 1;
	}
	/* ignore if same */
	if (file.size == size)
		return 0;
	if (ftruncate(f->fd, size)) {
		fprintf(stderr, "Can't truncate: %s\n", strerror(errno));
		return 1;
	}
	size_t oldsize = f->size;
	/* mremap ensures that data is kept intact even if it has been moved */
	void *map = mremap(f->data, f->size, size, MREMAP_MAYMOVE);
	if (map == MAP_FAILED) {
		fprintf(stderr, "Can't remap: %s\n", strerror(errno));
		return 1;
	}
	/* update mapping */
	f->data = map;
	f->size = size;
	if (msync(f->data, size, MS_SYNC)) {
		/*
		 * journaling may not be supported by the underlying filesystem
		 * try to revert to old state
		 */
		void *oldmap = mremap(f->data, size, oldsize, MREMAP_MAYMOVE);
		if (oldmap != MAP_FAILED) {
			f->data = oldmap;
			f->size = oldsize;
			fprintf(stderr,
				"Can't sync with file, journaling may be unsupported.\n"
				"Filesystems that do not support journaling are e.g.: fat, ext, nfts\n"
				"Sync error: %s\n",
				strerror(errno)
			);
			return 1;
		}
		/* give up, something is terribly broken */
		fprintf(stderr,
			"Can't sync with file, I/O broken: %s\n"
			"Going into readonly mode!\n",
			strerror(errno)
		);
		f->flags |= BF_READONLY;
		return 1;
	}
	if (fstat(f->fd, &f->st)) {
		fprintf(stderr, "Can't access %s: %s\n", f->name, strerror(errno));
		return 1;
	}
	return 0;
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
		if (!pos || pos >= base_str + base)
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
	case 'a':
		puts(help_poke);
		break;
	case 'c':
		puts(help_copy);
		break;
	case 'g':
		puts(help_showinfo);
		break;
	case 'h':
		puts(help_hunt);
		break;
	case 'm':
		puts(help_peek);
		break;
	case 'q':
		puts(help_quit);
		break;
	case 't':
		puts(help_truncate);
		break;
	default:
		return -1;
	}
	return 0;
}

static unsigned byte_count(uint64_t v)
{
	if (v > UINT32_MAX)
		return 8;
	if (v > UINT16_MAX)
		return 4;
	if (v > UINT8_MAX)
		return 2;
	return 1;
}

static int poke(char *start)
{
	char copy[INPUT_BUFSZ];
	char *token, *saveptr, *str;
	uint8_t *dest;
	uint64_t addr, value, mask, size;
	unsigned n = 0;
	if (file.flags & BF_READONLY) {
		fputs("Can't poke: readonly file\n", stderr);
		return 1;
	}
	strcpy(copy, start);
	/* first pass */
	for (str = copy; ; str = NULL) {
		token = strtok_r(str, SPACE_DELIM, &saveptr);
		if (!token)
			break;
		switch (n++) {
		case 0:
			if (parse_address(token, &addr, &mask)) {
				fputs("Bad address\n", stderr);
				return 1;
			}
			break;
		default:
			if (parse_address(token, &value, &mask)) {
				fprintf(stderr, "Bad value: %s\n", token);
				return 1;
			}
			n += byte_count(value) - 1;
			break;
		}
	}
	if (n < 2) {
		if (!n)
			fputs("Missing address\n", stderr);
		else
			fputs("Missing data\n", stderr);
		return 1;
	}
	--n;
	size = file.size;
	if (addr > size) {
		uint64_t overflow = addr - size;
		printf(
			"Can't poke: %" PRIu64 " %s behind file\n",
			overflow, overflow == 1 ? "byte" : "bytes"
		);
		return 1;
	}
	if (addr + n > size) {
		uint64_t overflow = addr + n - size;
		printf(
			"Poke overflows by %" PRIu64 " %s\n",
			overflow, overflow == 1 ? "byte" : "bytes"
		);
		return 1;
	}
	/* second pass */
	for (dest = (uint8_t*)file.data + addr, str = start, n = 0; ; str = NULL) {
		token = strtok_r(str, SPACE_DELIM, &saveptr);
		if (!token)
			break;
		if (n++) {
			unsigned bytes;
			parse_address(token, &value, &mask);
			bytes = byte_count(value);
			switch (bytes) {
			case 8: *((uint64_t*)dest) = value; break;
			case 4: *((uint32_t*)dest) = value; break;
			case 2: *((uint16_t*)dest) = value; break;
			case 1: *dest = value; break;
			}
			dest += bytes;
		}
	}
	return 0;
}

static int copy(char *start)
{
	char *token, *saveptr, *str;
	uint8_t *dest, *src;
	uint64_t from, to, mask, length, size;
	unsigned n = 0;
	for (str = start; ; str = NULL) {
		token = strtok_r(str, SPACE_DELIM, &saveptr);
		if (!token)
			break;
		switch (n++) {
		case 0:
			if (parse_address(token, &to, &mask)) {
				fputs("Bad destination\n", stderr);
				return 1;
			}
			break;
		case 1:
			if (parse_address(token, &from, &mask)) {
				fputs("Bad source\n", stderr);
				return 1;
			}
			break;
		case 2:
			if (parse_address(token, &length, &mask)) {
				fputs("Bad length\n", stderr);
				return 1;
			}
			break;
		default:
			fputs("Too many arguments\n", stderr);
			return 1;
		}
	}
	if (n != 3) {
		switch (n) {
		case  2: fputs("Missing length\n", stderr); break;
		case  1: fputs("Missing source\n", stderr); break;
		default: fputs("Missing destination\n", stderr); break;
		}
		return 1;
	}
	size = file.size;
	if (from + length > size) {
		uint64_t overflow = from + length - size;
		printf(
			"Source overflows by %" PRIu64 " %s\n",
			overflow, overflow == 1 ? "byte" : "bytes"
		);
		return 1;
	}
	if (to + length > size) {
		uint64_t overflow = to + length - size;
		printf(
			"Destination overflows by %" PRIu64 " %s\n",
			overflow, overflow == 1 ? "byte" : "bytes"
		);
		return 1;
	}
	dest = (uint8_t*)file.data + to;
	src = (uint8_t*)file.data + from;
	memmove(dest, src, length);
	return 0;
}

static int hunt(char *start)
{
	char *token, *saveptr, *str, *opt_len;
	uint8_t *data, hunt_buf[INPUT_BUFSZ], *hunt_ptr;
	uint64_t addr, value, mask, length, size;
	unsigned n = 0, bytes;
	for (str = start, hunt_ptr = hunt_buf; ; str = NULL) {
		token = strtok_r(str, SPACE_DELIM, &saveptr);
		if (!token)
			break;
		switch (n++) {
		case 0:
			/* check for `,length' */
			opt_len = strchr(token, ',');
			if (opt_len)
				*opt_len++ = '\0';
			if (parse_address(token, &addr, &mask)) {
				fputs("Bad address\n", stderr);
				return 1;
			}
			if (!opt_len) {
				length = addr > file.size ? 0 : file.size - addr;
				continue;
			}
			if (parse_address(opt_len, &length, &mask)) {
				fputs("Bad length\n", stderr);
				return 1;
			}
			break;
		default:
			if (parse_address(token, &value, &mask)) {
				fprintf(stderr, "Bad value: %s\n", token);
				return 1;
			}
			bytes = byte_count(value);
			switch (bytes) {
			case 8: *((uint64_t*)hunt_ptr) = value; break;
			case 4: *((uint32_t*)hunt_ptr) = value; break;
			case 2: *((uint16_t*)hunt_ptr) = value; break;
			case 1: *hunt_ptr = value; break;
			}
			hunt_ptr += bytes;
			break;
		}
	}
	if (n < 2) {
		if (!n)
			fputs("Missing address\n", stderr);
		else
			fputs("Missing data\n", stderr);
		return 1;
	}
	size = file.size;
	if (addr > size) {
		uint64_t overflow = addr - size;
		printf(
			"Can't hunt: %" PRIu64 " %s behind file\n",
			overflow, overflow == 1 ? "byte" : "bytes"
		);
		return 1;
	}
	if (addr + length > size) {
		uint64_t overflow = addr + length - size;
		printf(
			"Hunt end marker overflows by %" PRIu64 " %s\n",
			overflow, overflow == 1 ? "byte" : "bytes"
		);
		return 1;
	}
	n = hunt_ptr - hunt_buf;
	if (addr + n > size)
		goto not_found;
	data = memmem((unsigned char*)file.data + addr, length, hunt_buf, n);
	if (data) {
		printf("%" PRIX64 "\n", (uint64_t)((unsigned char*)data - (unsigned char*)file.data));
		return 0;
	}
not_found:
	return 1;
}

static int peek(char *start)
{
	char *token, *saveptr, *str;
	unsigned n = 0;
	uint64_t addr, length = 16, mask;
	for (str = start; ; str = NULL) {
		token = strtok_r(str, SPACE_DELIM, &saveptr);
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

static int do_truncate(char *start)
{
	uint64_t size, mask;
	char *arg;
	for (arg = start; isspace(*arg); ++arg)
		;
	if (parse_address(arg, &size, &mask)) {
		fputs("Bad filesize\n", stderr);
		return 1;
	}
	return bfile_truncate(&file, size);
}

static int parse(char *start)
{
	char *op;
	for (op = start + 1; isspace(*op); ++op)
		;
	switch (*start) {
	case '?':
		return help(op);
	case 'a':
		return poke(op);
	case 'c':
		return copy(op);
	case 'g':
		bfile_showinfo(&file);
		break;
	case 'h':
		return hunt(op);
	case 'm':
		return peek(op);
	case 't':
		return do_truncate(op);
	default:
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int ret;
	char input[INPUT_BUFSZ];
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
