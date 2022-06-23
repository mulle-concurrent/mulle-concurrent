### 2.2.12

* change GLOBALs for Windows

### 2.2.11

* Various small improvements

### 2.2.10

* Various small improvements

### 2.2.9

* use github actions instead of travis, upgrade mulle-sde

### 2.2.8

* new mulle-sde project structure

### 2.2.7

* Various small improvements

### 2.2.6

* fix test sourcetree

### 2.2.5

* updated mulle-sde

### 2.2.4

* improved aba library `find_library`

### 2.2.3

* fix error value EINVAL on enumerate and improve comments detailing error codes

### 2.2.2

* modernized to mulle-sde with .mulle folder

### 2.2.1

* fix README, ensure newest aba is used

## 2.2.0

* added experimental register function to get or set a value


### 2.0.6

* remove obsolete file

### 2.0.5

* remove cruft, modernize test, modernize project

### 2.0.4

* modernized mulle-sde

### 2.0.3

* Various small improvements

### 2.0.2

* fix travis.yml

### 2.0.1

* Various small improvements

# 2.0.0

* migrated to mulle-sde
* made headernames hyphenated
* no longer distributed as a homebrew package

### 1.4.11

* Various small improvements

### 1.4.9

* support new mulle-tests

### 1.4.7

* fixed scion wrapper command

### 1.4.5

* follow mulle-configuration 3.1 changes and move .travis.yml to trusty

### 1.4.3

* Various small improvements

## 1.4.1

* adapt to mulle-configuration 2.0


### 1.3.5

* make it a cmake "C" project

### 1.3.3

* modernize


1.3.2
===

* community release


1.3.1
===

* if you init a pointerarray or hashmap with size == 0, initialization will be
done lazily, using a static empty storage first.

1.1.5
===

* merge community fixes

1.1.4
===

* improve readme, improved packaging

1.1.3
===

* merge community fixes

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
