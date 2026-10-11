// Compiles the vendored tree-sitter runtime amalgam. Some libcs (glibc)
// only expose le16toh/be16toh when a feature test macro is defined before
// <endian.h> is first included; defining it here, ahead of every header,
// is bulletproof regardless of how the target's flags were assembled.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "../vendor/tree-sitter/src/lib.c"
