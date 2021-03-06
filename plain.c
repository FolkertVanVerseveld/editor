/* Copyright 2017 Folkert van Verseveld. See COPYING for details */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "fs.h"
#include "string.h"

#define SPACE_DELIM " \f\n\r\t\v"
#define INPUT_BUFSZ 256

static const char *help_editor =
	"Line-based editor\n"
	"Commands:\n"
	"a start data...           Poke data\n"
	"c dest src length         Copy data\n"
	"f start length data...    Fill data\n"
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

static const char *help_fill =
	"f: f start length data...\n"
	"  Fill memory with DATA. The pattern is repeated up to LENGTH bytes which must\n"
	"  be an exact multiple of the number of bytes specified by DATA.";

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
static struct bfile file;

static int check_overflow(uint64_t pos, uint64_t max, const char *format)
{
	if (pos < max)
		return 0;
	uint64_t overflow = pos - max + 1;
	printf(format, overflow, overflow == 1 ? "byte" : "bytes");
	return 1;
}

static int help(const char *start)
{
	if (!*start)
		puts(help_editor);
	else if (!strcmp(start, "a"))
		puts(help_poke);
	else if (!strcmp(start, "c"))
		puts(help_copy);
	else if (!strcmp(start, "f"))
		puts(help_fill);
	else if (!strcmp(start, "g"))
		puts(help_showinfo);
	else if (!strcmp(start, "h"))
		puts(help_hunt);
	else if (!strcmp(start, "m"))
		puts(help_peek);
	else if (!strcmp(start, "q"))
		puts(help_quit);
	else if (!strcmp(start, "t"))
		puts(help_truncate);
	else
		return -1;
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

	if (!bfile_is_rdwr2(&file, "poke"))
		return 1;
	if (bfile_map(&file))
		return 1;

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
	size = bfile_size(&file);

	if (check_overflow(addr, size, "Can't poke: %" PRIu64 " %s behind file\n"))
		return 1;
	if (check_overflow(addr + n - 1, size, "Poke overflows by %" PRIu64 " %s\n"))
		return 1;

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
	size = bfile_size(&file);

	if (!length)
		return 0;

	if (check_overflow(from + length - 1, size, "Source overflows by %" PRIu64 " %s\n"))
		return 1;
	if (check_overflow(to + length - 1, size, "Destination overflows by %" PRIu64 " %s\n"))
		return 1;

	memmove((uint8_t*)file.data + to, (uint8_t*)file.data + from, length);
	return 0;
}

static int fill(char *start)
{
	char *token, *saveptr, *str;
	uint8_t *dest, fill_buf[INPUT_BUFSZ], *fill_ptr;
	uint64_t addr, length, value, mask, size;
	unsigned n = 0, bytes;

	size = bfile_size(&file);
	if (!size) {
		fputs("File empty\n", stderr);
		return 1;
	}
	if (!bfile_is_rdwr2(&file, "fill") || bfile_map(&file))
		return 1;

	for (str = start, fill_ptr = fill_buf; ; str = NULL) {
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
		case 1:
			if (parse_address(token, &length, &mask)) {
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
			case 8: *((uint64_t*)fill_ptr) = value; break;
			case 4: *((uint32_t*)fill_ptr) = value; break;
			case 2: *((uint16_t*)fill_ptr) = value; break;
			case 1: *fill_ptr = value; break;
			}
			fill_ptr += bytes;
			break;
		}
	}
	if (n < 3) {
		switch (n) {
		case 2:
			fputs("Missing data\n", stderr);
			break;
		case 1:
			fputs("Missing length\n", stderr);
			break;
		default:
			fputs("Missing address\n", stderr);
			break;
		}
		return 1;
	}
	n = fill_ptr - fill_buf;
	if (length % n) {
		fputs("Block is not multiple of length\n", stderr);
		return 1;
	}

	if (check_overflow(addr, size, "Can't fill: %" PRIu64 " %s behind file\n"))
		return 1;
	if (check_overflow(addr + length - 1, size, "Fill overflows by %" PRIu64 " %s\n"))
		return 1;

	for (dest = (uint8_t*)file.data + addr; length; length -= n, dest += n)
		memcpy(dest, fill_buf, n);
	return 0;
}

static int hunt(char *start)
{
	char *token, *saveptr, *str, *opt_len;
	uint8_t *data, hunt_buf[INPUT_BUFSZ], *hunt_ptr;
	uint64_t addr, value, mask, length, size;
	unsigned n = 0, bytes;

	if (bfile_map(&file))
		return 1;

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
				goto not_found;
			}
			if (!opt_len) {
				length = addr > bfile_size(&file) ? 0 : bfile_size(&file) - addr;
				continue;
			}
			if (parse_address(opt_len, &length, &mask)) {
				fputs("Bad length\n", stderr);
				goto not_found;
			}
			break;
		default:
			if (parse_address(token, &value, &mask)) {
				fprintf(stderr, "Bad value: %s\n", token);
				goto not_found;
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
		goto not_found;
	}
	size = bfile_size(&file);

	if (check_overflow(addr, size, "Can't hunt: %" PRIu64 " %s behind file\n"))
		goto not_found;
	if (check_overflow(addr + length - 1, size, "Hunt end marker overflows by %" PRIu64 " %s\n"))
		goto not_found;

	n = hunt_ptr - hunt_buf;
	if (addr + n > size)
		goto not_found;

	data = memmem((unsigned char*)file.data + addr, length, hunt_buf, n);
	if (data) {
		printf("$%" PRIX64 "\n", (uint64_t)((unsigned char*)data - (unsigned char*)file.data));
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

	if (bfile_map(&file))
		return 1;

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

static int do_truncate(const char *start)
{
	uint64_t size, mask;
	if (!start || parse_address(start, &size, &mask)) {
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
	case 'f':
		return fill(op);
	case 'g':
		bfile_showinfo(&file);
		break;
	case 'h':
		return hunt(op);
	case 'm':
		return peek(op);
	case 't':
		return do_truncate(cmd_next_arg(start));
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
	if (ret) {
		bfile_print_error(stderr, argv[1], ret);
		return 1;
	}
	while (fgets(input, sizeof input, stdin)) {
		str_trim(input);
		if (!strcmp("q", input))
			break;
		if (parse(input) < 0)
			fprintf(stderr, "? %s\n", input);
	}
	bfile_close(&file);
	return 0;
}
