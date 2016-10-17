# mulle-concurrent

**mulle-concurrent** is a library for wait-free data structures. Wait-freeness
is a desirable property for "hotly" contested data structures in multi-threaded
environments.


## Data structures

Name                            | Description
--------------------------------|---------------------------------------------
`mulle_concurrent_hashmap`      | A growing, mutable map of pointers, indexed by a hash
`mulle_concurrent_pointerarray` | A growing array of pointers



> Most of the ideas are taken from [Preshing on Programming](http://preshing.com/20160222/a-resizable-concurrent-map/).
> The definition of concurrent and wait-free are from [concurrencyfreaks.blogspot.de](http://concurrencyfreaks.blogspot.de/2013/05/lock-free-and-wait-free-definition-and.html)


## Install

On OS X and Linux you can use
[homebrew](//brew.sh), respectively
[linuxbrew](//linuxbrew.sh)
to install the library:

```
brew tap mulle-kybernetik/software
brew install mulle-concurrent
```

On other platforms you can use **mulle-install** from
[mulle-build](//www.mulle-kybernetik.com/software/git/mulle-build)
to install the library:

```
mulle-install --prefix /usr/local --branch release https://www.mulle-kybernetik.com/repositories/mulle-concurrent
```


Otherwise read:

* [How to Build](dox/BUILD.md)


## API

* [mulle_concurrent_pointerarray](dox/API_POINTERARRAY.md)
* [mulle_concurrent_hashmap](dox/API_HASHMAP.md)


### Platforms and Compilers

All platforms and compilers supported by
[mulle-c11](//www.mulle-kybernetik.com/software/git/mulle-c11/) and
[mulle-thread](//www.mulle-kybernetik.com/software/git/mulle-thread/).


## Author

[Nat!](//www.mulle-kybernetik.com/weblog) for
[Mulle kybernetiK](//www.mulle-kybernetik.com) and
[Codeon GmbH](//www.codeon.de)