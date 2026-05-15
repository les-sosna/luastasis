/*
** luaser_tojson.h
** Public API for the luaser binary-to-JSON converter.
*/

#ifndef luaser_tojson_h
#define luaser_tojson_h

#include <stdio.h>
#include <stddef.h>

/*
** Convert a luaser binary buffer to a human-readable JSON representation
** written to 'out'.  Returns 0 on success, 1 on error.
*/
int luaser_tojson(const unsigned char *buf, size_t size, FILE *out);

#endif /* luaser_tojson_h */
