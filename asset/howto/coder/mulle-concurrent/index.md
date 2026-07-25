<!-- Keywords: hashmap, pointerarray, pointerset, sentinels, enumeration, allocator -->

# mulle-concurrent — coder bundle

Use when writing C code that needs lock- and wait-free concurrent data
structures (`mulle_concurrent_hashmap`, `mulle_concurrent_pointerarray`,
`mulle_concurrent_pointerset`) in a multi-threaded context.

## Understand first

```bash
mulle-sde api apropos concurrent
mulle-sde api cat mulle-concurrent-hashmap
mulle-sde api cat mulle-concurrent-pointerarray
mulle-sde api cat mulle-concurrent-pointerset
mulle-sde howto show --role coder --topic mulle-concurrent
```

## Core API families

| Family | Header | Purpose |
|---|---|---|
| `mulle_concurrent_hashmap` | `src/hashmap/mulle-concurrent-hashmap.h` | Wait-free hash-table mapping `intptr_t` hash → `void *` value |
| `mulle_concurrent_pointerarray` | `src/pointerarray/mulle-concurrent-pointerarray.h` | Wait-free grow-only pointer array |
| `mulle_concurrent_pointerset` | `src/pointerset/mulle-concurrent-pointerset.h` | Wait-free pointer set with linear probing |
| Types / sentinels | `src/mulle-concurrent-types.h` | `MULLE_CONCURRENT_NO_HASH`, `*_INVALID_POINTER`, `*_NO_POINTER`, `*_TOMBSTONE_POINTER` |

## Scenario to API map

| Scenario | Use |
|---|---|
| Map integer keys to pointers, concurrent r/w | `mulle_concurrent_hashmap` |
| Append-only concurrent pointer list | `mulle_concurrent_pointerarray` |
| Deduplicate pointers concurrently | `mulle_concurrent_pointerset` |
| Iterate all entries | `mulle_concurrent_*_enumerate` / `for` macros |
| Custom memory management per structure | Pass a `struct mulle_allocator *` to `_init` |

## Primary multi-threaded workflow

1. Call `mulle_aba_init(allocator)` **once** at process start.
2. Every thread that touches any concurrent structure **must** call
   `mulle_aba_register()` before use and `mulle_aba_unregister()` when done.
3. Call `mulle_concurrent_*_init` once in a **single-threaded** context.
4. Use register/insert/lookup/remove from any thread (wait-free).
5. Call `mulle_concurrent_*_done` once in a **single-threaded** context.
6. Call `mulle_aba_done()` **once** at process end.

Enumeration is **limited multi-threaded**: safe only when no concurrent
mutations occur, or when the caller handles `ECANCELLED`/`EBUSY` via retry.
Enumerators are **thread-local** — never share them across threads.

## Local references

- `asset/dox/TOC.md` — full API reference and examples
- `src/mulle-concurrent.h` — umbrella header (version `3.2.0`)
- `src/mulle-concurrent-types.h` — sentinel values
- `src/hashmap/mulle-concurrent-hashmap.h` — hashmap API
- `src/pointerarray/mulle-concurrent-pointerarray.h` — pointerarray API
- `src/pointerset/mulle-concurrent-pointerset.h` — pointerset API
- `test/hashmap/hashmap.c` — hashmap stress test (single + multi-threaded)
- `test/hashmap/example.c` — hashmap insert/lookup/remove/enum demo
- `test/hashmap/othersimple.c` — hashmap insert/lookup/remove/enum demo
- `test/array/pointerarray.c` — pointerarray stress test (single + multi-threaded)
- `test/array/example.c` — minimal pointerarray demo
- `test/array/simple.c` — pointerarray get/enumerate demo
- `test/array/empty.c` — empty pointerarray state checks
