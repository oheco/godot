# oheco-broker C SDK

Source: https://github.com/oheco/oheco-broker

Vendored entirely from the existing local checkout at
`/storage/Users/currentUser/dev/ohos/oheco-broker`, without downloading sources.
Pinned local Git HEAD: `aae5996f00f3a24dfc62a39331d7878c05933355`.
The copied files were unmodified relative to HEAD; unrelated local changes in
`internal/control` and `tests/remote_acceptance.py` were not included.

| Original path | Git blob at HEAD | SHA-256 of copied file |
| --- | --- | --- |
| sdk/c/oheco_broker.c | b551e27cc3d0289ab6eabf8b4d3246062b61dbc3 | 6f80299f0f1f15b29888ff6ec8427e8ae6a1fd6a05a3421a951cd5eb69dd5a3d |
| sdk/c/oheco_broker.h | ba12253db9c05c0238355cdadb26915dc3e56da2 | 42dce280339bc9914f00dda80786fbdd86e8f9432eeeeaf1b39a8de9cef23ac8 |
| LICENSE | dfba896291ba13fdd035957d94375cf49abfc9f8 | 3967125f32644b684201614a657775bd4f21bd5a4bd052fa9eec0db2479ad9c7 |

License: MIT, original copyright and LICENSE preserved.
Local modifications: none. The original endpoint-file API is retained; there is
no port API or protocol fork. Only OpenHarmony editor/export builds compile this
C source. The separate Godot adapter uses managed requests and does not retry.
No transport SDK, generated binary, or unrelated broker sources are required.
