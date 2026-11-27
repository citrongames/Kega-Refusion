# Lua 5.5.1

The upstream source directory is vendored unchanged from the
[Lua 5.5.1 release](https://www.lua.org/ftp/lua-5.5.1.tar.gz).
Its SHA-256 is `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`.

Kega Refusion statically links a subset of the source files and builds them
as C++ so native binding frames unwind on Lua errors. The command-line
tools and filesystem, OS, package, debug and coroutine libraries are excluded
from the linked subset. The MIT notice is retained in `LICENSE` and `src/lua.h`.
