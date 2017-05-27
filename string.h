#ifndef STRING_H
#define STRING_H

#include <stdint.h>

int parse_address(const char *str, uint64_t *address, uint64_t *mask);

#endif
