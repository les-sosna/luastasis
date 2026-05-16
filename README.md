# LuaStasis

A deterministic Lua runtime with built-in VM persistence and replayable execution.

LuaStasis is a fork of Lua 5.5 that adds first-class support for snapshotting and restoring the complete state of a Lua VM — including all objects, closures, upvalues, coroutine stacks, and call frames — to and from a compact binary format. The goal is a runtime where any execution point can be captured, stored, replicated, or replayed.

## What it adds

- **`lstasis_save`** — serialize a live `lua_State` to a memory buffer with zero impact on the caller's state.
- **`lstasis_load`** — restore a fully operational `lua_State` from a previously saved buffer.
- **`lstasis_tojson`** — CLI tool for human-readable inspection of snapshot files.

## Serialization API

```c
#include "lstasis.h"

/* Snapshot a running state into a heap-allocated buffer. */
int lstasis_save(lua_State *L,
                const lstasis_Lib *libs,   /* registered C libraries */
                unsigned char **out_buf,
                size_t        *out_size);

/* Restore a state from a snapshot buffer. */
lua_State *lstasis_load(const unsigned char *buf,
                       size_t               size,
                       const lstasis_Lib    *libs);
```

`libs` is a NULL-terminated array that tells the serializer how to name and re-resolve C functions across save/load boundaries:

```c
static const lstasis_Lib my_libs[] = {
    {"base",   luaopen_base},
    {"table",  luaopen_table},
    {"string", luaopen_string},
    /* ... */
    {NULL, NULL}
};

/* Save */
unsigned char *buf = NULL;
size_t sz = 0;
lstasis_save(L, my_libs, &buf, &sz);

/* Restore on the same or a different process */
lua_State *L2 = lstasis_load(buf, sz, my_libs);
free(buf);
```

What survives a round-trip:

- All Lua values: nil, booleans, integers, floats, strings, tables (including cyclic references and metatables)
- Lua closures with their prototypes (bytecode) and upvalues, including shared upvalues
- C closures and light C functions (resolved by name through `libs`)
- Coroutines at any yield point, with full call stack and locals restored
- Open upvalues referencing live stack slots

## Inspecting snapshots

`lstasis_tojson` converts a binary snapshot to pretty-printed JSON for debugging:

```sh
# Build
make lstasis_tojson

# Capture a snapshot from your program and inspect it
./lstasis_tojson snapshot.bin | less

# Or pipe directly
./my_program | ./lstasis_tojson
```

The JSON output lists every object in the snapshot by ID with its type, contents, and inter-object references, making it straightforward to verify what was captured or to diff two snapshots.

## Building

LuaStasis uses the same developer Makefile as upstream Lua:

```sh
make          # builds liblua.a, lua, test_serial, lstasis_tojson
make test_serial && ./test_serial   # run the serialization test suite
```

Requires GCC and standard POSIX libraries. Tested on Linux.

## Upstream

LuaStasis is based on [Lua 5.5](https://www.lua.org/). The Lua language, VM, and standard libraries are the work of PUC-Rio. See [lua.h](lua.h) for the original copyright notice.
