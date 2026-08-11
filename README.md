# mulle-concurrent

#### 📶 A lock- and wait-free hashtable, pointer array and pointer set, written in C

**mulle-concurrent** is a library for lock- and wait-free data structures.
Wait-freeness is a desirable property for "hotly" contested data structures
in multi-threaded environments.

> Many of the ideas are taken from [Preshing on Programming: A Resizable, Concurrent Map](http://preshing.com/20160222/a-resizable-concurrent-map/).
> The definition of concurrent and wait-free are from [concurrencyfreaks.blogspot.de](http://concurrencyfreaks.blogspot.de/2013/05/lock-free-and-wait-free-definition-and.html)



| Release Version                                       | Release Notes  | AI Documentation
|-------------------------------------------------------|----------------|---------------
| ![Mulle kybernetiK tag](https://img.shields.io/github/tag/mulle-concurrent/mulle-concurrent.svg) [![Build Status](https://github.com/mulle-concurrent/mulle-concurrent/workflows/CI/badge.svg)](//github.com/mulle-concurrent/mulle-concurrent/actions) | [RELEASENOTES](RELEASENOTES.md) | [DeepWiki for mulle-concurrent](https://deepwiki.com/mulle-concurrent/mulle-concurrent)


## API

| Data Structure                                    | Description
| --------------------------------------------------|-----------------------------------
| [`mulle-concurrent-hashmap`](dox/API_HASHMAP.md)  | A wait and lock free hashmap
| [`mulle-concurrent-pointerarray`](dox/API_POINTERARRAY.md)  | A wait and lock free array
| [`mulle-concurrent-pointerset`](dox/API_POINTERSET.md)  | A wait and lock free set of pointers





## Documentation & Guides

* [API Summary](asset/dox/api/toc)
* [Coder Guide](asset/howto/coder/mulle-concurrent)

## Usage

mulle-concurrent data structures require
[mulle-aba](//github.com/mulle-concurrent/mulle-aba) for safe concurrent memory
reclamation. You must initialize mulle-aba once per process and **register every
thread** that accesses mulle-concurrent data structures.

``` c
#include <mulle-concurrent/mulle-concurrent.h>

int   main( int argc, char *argv[])
{
   struct mulle_concurrent_hashmap   map;

   mulle_aba_init( NULL);                          // once per process
   mulle_aba_register();                           // register main thread

   mulle_concurrent_hashmap_init( &map, 0, NULL);

   // ... use the hashmap ...

   mulle_concurrent_hashmap_done( &map);

   mulle_aba_unregister();                         // unregister main thread
   mulle_aba_done();                               // once per process
   return( 0);
}
```

### Multi-threaded setup

**Every participating thread** must call `mulle_aba_register` before accessing
any mulle-concurrent data structure and `mulle_aba_unregister` before exiting.
Forgetting to do so will crash.

With `mulle_thread` you can automate the unregister with a TSS destructor:

``` c
#include <mulle-concurrent/mulle-concurrent.h>

static mulle_thread_tss_t   aba_tss_key;


static void   aba_thread_destructor( void *value)
{
   mulle_aba_unregister();
}


// call once from main before spawning threads
static void   aba_setup_thread_cleanup( void)
{
   mulle_thread_tss_create( aba_thread_destructor, &aba_tss_key);
}


// call at the start of each thread function
static void   aba_register_thread( void)
{
   mulle_aba_register();
   mulle_thread_tss_set( aba_tss_key, (void *) 1);  // non-NULL triggers destructor
}
```

Then your thread function becomes:

``` c
static void   *my_worker( void *arg)
{
   struct mulle_concurrent_hashmap   *map = arg;

   aba_register_thread();

   // ... safely use map ...

   return( NULL);  // destructor calls mulle_aba_unregister automatically
}
```


## Lifecycle checklist

`init` and `done` must be externally serialized, and every thread that touches
a mulle-concurrent data structure must be registered with mulle-aba. The
complete process looks like this:

1. **Initialize mulle-aba once per process** — `mulle_aba_init( allocator)`.
2. **Configure the allocator for ABA reclamation** — if you pass a custom
   allocator, wire it up with `mulle_allocator_set_aba()` so that old storage
   can be freed safely.
3. **Register every participating thread** — `mulle_aba_register()` before the
   first access, `mulle_aba_unregister()` when the thread is done. In a
   thread pool, use the TSS destructor recipe from *Multi-threaded setup*
   above to make this automatic.
4. **`init` your structures** — in a single-threaded context, before any
   worker threads start.
5. **Use the structures** — from any registered thread.
6. **Stop the world** — ensure no thread accesses the structures anymore
   (join the workers / drain the pool) before you tear anything down.
7. **`done` your structures** — in a single-threaded context, after the stop.
8. **Unregister the threads and finish mulle-aba** — `mulle_aba_done()` once
   per process, after all accesses have ceased.

Destroying a structure while a reader is still active is not recoverable. If
a thread accesses a structure without being registered, the process will
crash.


## Concurrency semantics

* **Point operations are wait-free** — `register`, `insert`, `lookup`,
  `remove`, `add`, `get`, `member` complete in a bounded number of steps
  regardless of contention. Growth and migration are cooperative: any thread
  that observes a REDIRECT slot helps finish the migration and then retries
  its own operation.
* **`count`, `get_size`, `get_count` and `lookup_any` are snapshots** — they
  return a value from some point in time during the call and may be stale if
  the structure is mutated concurrently. Use them for diagnostics and
  heuristics, not for correctness decisions.
* **Enumeration is "limited multi-threaded"** — an enumerator is only usable
  by the calling thread. For the hashmap and pointerset, concurrent mutation
  (insert/remove/growth) may interrupt the enumeration; the enumerator then
  returns `ECANCELLED` and you retry the whole enumeration from the start.
  Retrying may yield duplicates or miss entries that changed in between —
  that is expected. The pointerarray is append-only, so its enumerators work
  reliably even while other threads add values.
* **`remove` on a hashmap requires the matching value** — the pair (hash,
  value) must match, so a removed entry cannot be resurrected by a newer
  value. The slot is left as a tombstone: the hash stays claimed (so probe
  chains through it keep working) but the value is marked removed. A later
  `register`/`insert` of the same key refills the tombstone in place;
  tombstones for keys that are not re-registered are dropped at the next
  migration.
* **Hashmap `register`/`insert` never return a foreign key's value** — a
  slot is claimed by CASing its hash, and only the thread holding that claim
  may write its value, so two different keys can never collide on one
  slot's value.
* **Pointerset tombstones accumulate** — a remove-heavy workload migrates
  early; use `mulle_concurrent_pointerset_reset()` to reclaim. The hashmap's
  tombstones do not need this: migration sizing accounts for them, so a
  remove/register churn workload compacts instead of growing without bound.
* **`init` and `done` are single-threaded** — no other thread may access the
  structure while either runs. See the *Lifecycle checklist*.


## Reserved pointer values

The containers store `void *` pointers and reserve a few values for internal
use. **Never store any of these values as payload.**

| Value                 | Constant                            | Meaning                           | Rejected by
|-----------------------|-------------------------------------|-----------------------------------|-------------------
| `0` (hash)            | `MULLE_CONCURRENT_NO_HASH`          | invalid hash sentinel             | hashmap insert/register/remove
| `NULL`                | `MULLE_CONCURRENT_NO_POINTER`       | "no value" sentinel / empty slot  | all structures
| `(void *) INTPTR_MIN` | `MULLE_CONCURRENT_INVALID_POINTER`  | REDIRECT marker during migration  | all structures
| `(void *) INTPTR_MAX` | `MULLE_CONCURRENT_TOMBSTONE_POINTER`| tombstone of a removed slot       | hashmap, pointerset

The values are defined in `src/mulle-concurrent-types.h`. They are
compile-time constants derived from the platform's `intptr_t`, so the
reserved set is fixed per architecture. Passing a reserved value to an
operation that rejects it returns `EINVAL`.


## Memory allocation

All allocation is **fail-fast**: the mulle allocator contract is *success or
abort*. When the default allocator cannot satisfy an allocation it prints an
error and aborts the process. The API therefore only reports allocation
failure by aborting; do not write code that depends on recovering from it.



### You are here

![Overview](overview.dot.svg)





## Add

**This project is a component of the [mulle-core](//github.com/mulle-core/mulle-core) library. As such you usually will *not* add or install it individually, unless you specifically do not want to link against `mulle-core`.**


### Add as a git submodule

mulle-concurrent is intended to be consumed as a git submodule with CMake's `add_subdirectory()` — no mulle tooling is required in the parent build:

``` sh
git submodule add https://github.com/mulle-concurrent/mulle-concurrent.git \
   third_party/mulle-concurrent
```

Add this to your `CMakeLists.txt`:

``` cmake
add_subdirectory( third_party/mulle-concurrent)
target_link_libraries( my_target PRIVATE mulle-concurrent)
```

``` c
#include <mulle-concurrent/mulle-concurrent.h>
```

The parent project must supply the mulle dependency graph — `mulle-aba` and its transitive targets (e.g. by also adding `mulle-core`) — so that the dependency resolves to CMake targets rather than system libraries.


### Add with mulle-sde (mulle ecosystem)

Use [mulle-sde](//github.com/mulle-sde) to add mulle-concurrent to your project:

``` sh
mulle-sde add github:mulle-concurrent/mulle-concurrent
```


### Embed with clib (sources only)

To only add the sources of mulle-concurrent with dependency sources use [clib](https://github.com/clibs/clib):

``` sh
clib install --out src/mulle-concurrent mulle-concurrent/mulle-concurrent
```

Add `-isystem src/mulle-concurrent` to your `CFLAGS` and compile all the sources that were downloaded with your project.

## Install

Use [mulle-sde](//github.com/mulle-sde) to build and install mulle-concurrent and all dependencies:

``` sh
mulle-sde install --prefix /usr/local \
   https://github.com/mulle-concurrent/mulle-concurrent/archive/latest.tar.gz
```

### Legacy Installation

Install the requirements:

| Requirements                                 | Description
|----------------------------------------------|-----------------------
| [mulle-aba](https://github.com/mulle-concurrent/mulle-aba)             | 🚮 A lock-free, cross-platform solution to the ABA problem

Download the latest [tar](https://github.com/mulle-concurrent/mulle-concurrent/archive/refs/tags/latest.tar.gz) or [zip](https://github.com/mulle-concurrent/mulle-concurrent/archive/refs/tags/latest.zip) archive and unpack it.

Install **mulle-concurrent** into `/usr/local` with [cmake](https://cmake.org):

``` sh
PREFIX_DIR="/usr/local"
cmake -B build                               \
      -DMULLE_SDK_PATH="${PREFIX_DIR}"       \
      -DCMAKE_INSTALL_PREFIX="${PREFIX_DIR}" \
      -DCMAKE_PREFIX_PATH="${PREFIX_DIR}"    \
      -DCMAKE_BUILD_TYPE=Release &&
cmake --build build --config Release &&
cmake --install build --config Release
```


## Author

[Nat!](https://mulle-kybernetik.com/weblog) for Mulle kybernetiK  



