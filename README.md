# mulle-concurrent

#### 📶 A lock- and wait-free hashtable (and an array too), written in C

**mulle-concurrent** is a library for lock- and wait-free data structures.
Wait-freeness is a desirable property for "hotly" contested data structures
in multi-threaded environments.

> Many of the ideas are taken from [Preshing on Programming: A Resizable, Concurrent Map](http://preshing.com/20160222/a-resizable-concurrent-map/).
> The definition of concurrent and wait-free are from [concurrencyfreaks.blogspot.de](http://concurrencyfreaks.blogspot.de/2013/05/lock-free-and-wait-free-definition-and.html)


| Release Version
|-----------------------------------
|[![Build Status](https://github.com/mulle-concurrent/mulle-concurrent/workflows/CI/badge.svg?branch=release)](//github.com/mulle-concurrent/mulle-concurrent)  ![Mulle kybernetiK tag](https://img.shields.io/github/tag/mulle-concurrent/mulle-concurrent/workflows/CI/badge.svg?branch=release)

## Data structures

API                                                   | Description    | Example
------------------------------------------------------|----------------|---------
[`mulle_concurrent_hashmap`](dox/API_HASHMAP.md)      | A growing, mutable map of pointers, indexed by a hash. A.k.a. hashtable, dictionary, maptable  | [Example](test/hashmap/example.c)
[`mulle_concurrent_pointerarray`](dox/API_POINTERARRAY.md) | A growing array of pointers                                                   | [Example](test/array/example.c)



### You are here

![Overview](overview.dot.svg)


## Add

Use [mulle-sde](//github.com/mulle-sde) to add mulle-concurrent to your project:

``` sh
mulle-sde dependency add --c --github mulle-concurrent mulle-concurrent
```

## Install

### mulle-sde

Use [mulle-sde](//github.com/mulle-sde) to build and install mulle-concurrent and all dependencies:

``` sh
mulle-sde install --prefix /usr/local \
   https://github.com/mulle-concurrent/mulle-concurrent/archive/latest.tar.gz
```

### Manual Installation


Install the requirements:

Requirements                                         | Description
-----------------------------------------------------|-----------------------
[mulle-aba](//github.com/mulle-concurrent/mulle-aba) | Concurrent memory reclamation


Install into `/usr/local`:

``` sh
cmake -B build \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DCMAKE_PREFIX_PATH=/usr/local \
      -DCMAKE_BUILD_TYPE=Release &&
cmake --build build --config Release &&
cmake --install build --config Release
```

### Platforms and Compilers

All platforms and compilers supported by
[mulle-c11](//github.com/mulle-c/mulle-c11) and
[mulle-thread](//github.com/mulle-concurrent/mulle-thread).


## Author

[Nat!](//www.mulle-kybernetik.com/weblog) for
[Mulle kybernetiK](//www.mulle-kybernetik.com) and
[Codeon GmbH](//www.codeon.de)
