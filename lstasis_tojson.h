/*
** lstasis_tojson.h
** LuaStasis: public API for the snapshot-to-JSON converter.
*/

#ifndef lstasis_tojson_h
#define lstasis_tojson_h

#include <stdio.h>
#include <stddef.h>

/*
** Convert a lstasis snapshot buffer to a human-readable JSON representation
** written to 'out'.  Returns 0 on success, 1 on error.
*/
int lstasis_tojson(const unsigned char *buf, size_t size, FILE *out);

#endif /* lstasis_tojson_h */
