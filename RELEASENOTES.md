1.1.2
===

* fixes for community version

1.1.1
===

* fix a test

1.1.0
===

* recompile because `mulle_allocator` change dramatically in layout
* change some int returning funtions to void. Will probably redo the error
handling here. Does this warrant a major version ? Hmm...


1.0.1-1.0.8
===

* fix packaging
* improve documentation
* use find_library in CMakeLists.txt


# v1.0

* renamed `_mulle_concurrent_hashmap_lookup_any` to `mulle_concurrent_hashmap_lookup_any` since its safe to pass NULL.
* renamed `_mulle_concurrent_hashmap_get_count` to `mulle_concurrent_hashmap_count`,
since it's safe to pass NULL and it's not a get operation.'
* improved the documentation
* added  some more "safe API" routines for release
* improved the headers for readability
* clarified return codes of `mulle_concurrent_hashmap_remove`.

# v0.5

* changed internal representation of mask from unsigned int to uintptr_t,
  because it's easier to read by the debugger

# v0.4

* does not use `errno` directly anymore, but instead returns the errno codes
  as the return value (sometimes as negative numbers)

# v0.3

* change init error code to EINVAL, because that's what the other code uses.
* fix some gcc compile problems

# v0.2

* Adapt to changes in `mulle_allocator` and `mulle_aba`

# v0.1

* Remove dependency on `mulle_aba` for the pure library.
* Rename _free to _done.

# v0.0

* Merciful Release
