/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The map pack payload serialised in C from the compiled tables (refpack_fixture.c), to a file: tools/test_datapack.py
 * compares it with tools/refpack.py's, section by section (docs/mappack-design.md G T3). */
#include <stdio.h>
#include "refpack_fixture.h"

int main(int argc, char **argv)
{
	static uint8_t p[REFPACK_MAX];
	size_t n = refpack_fixture_payload(p, sizeof p, REFPACK_ALL);
	FILE *f = argc > 1 ? fopen(argv[1], "wb") : NULL;
	if (n == 0u || f == NULL || fwrite(p, 1, n, f) != n)
		return 1;
	fclose(f);
	printf("refpack_dump: %zu bytes\n", n);
	return 0;
}
