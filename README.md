# mulle-concurrent

**mulle-concurrent** is a library for lock- and wait-free data structures.
Wait-freeness is a desirable property for "hotly" contested data structures
in multi-threaded environments.

> Many of the ideas are taken from [Preshing on Programming: A Resizable, Concurrent Map](http://preshing.com/20160222/a-resizable-concurrent-map/).
> The definition of concurrent and wait-free are from [concurrencyfreaks.blogspot.de](http://concurrencyfreaks.blogspot.de/2013/05/lock-free-and-wait-free-definition-and.html)


Fork      |  Build Status | Release Version
----------|---------------|-----------------------------------
[Mulle kybernetiK](//github.com/mulle-c/mulle-concurrent) | [![Build Status](https://travis-ci.org/mulle-c/mulle-concurrent.svg?branch=release)](https://travis-ci.org/mulle-c/mulle-concurrent) | ![Mulle kybernetiK tag](https://img.shields.io/github/tag/mulle-c/mulle-concurrent.svg) [![Build Status](https://travis-ci.org/mulle-c/mulle-concurrent.svg?branch=release)](https://travis-ci.org/mulle-c/mulle-concurrent)


## Install

Install the prerequisites first:

| Prerequisites                               |
|---------------------------------------------|
| [mulle-aba](//github.com/mulle-c/mulle-aba) |

Then build and install

```
mkdir build 2> /dev/null
(
   cd build ;
   cmake .. ;
   make install
)
```

Or let [mulle-sde](//github.com/mulle-sde) do it all for you.


## Data structures

API                                                   | Description    | Example
------------------------------------------------------|----------------|---------
[`mulle_concurrent_hashmap`](dox/API_POINTERARRAY.md) | A growing, mutable map of pointers, indexed by a hash. A.k.a. hashtable, dictionary, maptable | [Example](tests/hashmap/example.c)
[`mulle_concurrent_pointerarray`](dox/API_HASHMAP.md) | A growing array of pointers                                                               | [Example](tests/array/example.c)


### Platforms and Compilers

All platforms and compilers supported by
[mulle-c11](//github.com/mulle-c/mulle-c11) and
[mulle-thread](//github.com/mulle-c/mulle-thread).


## Author

[Nat!](//www.mulle-kybernetik.com/weblog) for
[Mulle kybernetiK](//www.mulle-kybernetik.com) and
[Codeon GmbH](//www.codeon.de)
