/* Copyright 2017 Folkert van Verseveld. See COPYING for details */
#include "string.h"
#include <ctype.h>
#include <string.h>

int parse_address(const char *str, uint64_t *address, uint64_t *mask)
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

char *str_trim(char *str)
{
	char *start, *end;
	for (start = str; ; ++start)
		if (!isspace(*start))
			break;
	for (end = start + strlen(start); end > start; --end)
		if (!isspace(end[-1]))
			break;
	memmove(str, start, end - start);
	end -= start - str;
	*end = '\0';
	return str;
}

char *cmd_next_arg(const char *str)
{
	const unsigned char *ptr = (const unsigned char*)str;
	while (*ptr && !isspace(*ptr))
		++ptr;
	while (*ptr && isspace(*ptr))
		++ptr;
	return *ptr ? (char*)ptr : NULL;
}
