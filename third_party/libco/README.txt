libco x86-64 cooperative contexts from https://github.com/libretro/libretro-common
Commit d9597c359a5ca8399a4e244f816e7e975a92a2aa
amd64.c: byuu, public domain. Headers retain their MIT notices.
Used only for an active MD Link Cable pair, on the emulation worker thread.
Local Win64 corrections: explicitly align the primary TLS context to 16 bytes
(MinGW emutls otherwise requests 8); reserve the 32-byte argument home area
inside each initial stack; abort if a context entrypoint returns in release.
