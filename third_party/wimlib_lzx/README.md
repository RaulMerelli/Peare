# wimlib LZX subset

Isolated GPLv3 LZX decoder subset imported from wimlib for Peare (AGPLv3).
Upstream: https://wimlib.net/ and https://github.com/ebiggers/wimlib

The code is intentionally kept outside the XEX parser.  XEX container framing is
handled by `src/core/XexModule.cpp`.  The wimlib decoder currently
implements the WIM LZX framing; the XEX/CAB-compatible stream adapter is kept as
a separate integration step rather than patching container logic into vendored code.

## Peare frontend boundary

`include/peare/lzx_frontends.h` exposes independent WIM, XEX and CAB frontend
APIs.  The original WIM decoder remains unchanged.  XEX has a separate,
stateful entry point because its output may exceed the physical LZX window and
state must continue across the de-blocked stream.  CAB has a reserved entry
point for future container support.
