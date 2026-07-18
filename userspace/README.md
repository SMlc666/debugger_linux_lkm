# liblkmdbg

`liblkmdbg` is the supported C user-space interface for the lkmdbg kernel
module. It owns protocol initialization, file descriptors, mapped resources,
partial-transfer reporting, and event polling. The raw ioctl protocol remains
available through `lkmdbg_raw_ioctl()` for features that do not yet have a
typed wrapper.

## Build

```sh
cmake -S userspace -B build/userspace -G Ninja
cmake --build build/userspace
ctest --test-dir build/userspace --output-on-failure
cmake --install build/userspace --prefix /usr/local
```

Both `liblkmdbg.so` and `liblkmdbg.a` are produced. Installed consumers can use
either CMake's `lkmdbg::lkmdbg` / `lkmdbg::lkmdbg_static` targets or
`pkg-config liblkmdbg`.

## Example

```c
#include <lkmdbg/lkmdbg.h>

struct lkmdbg_session *session;
struct lkmdbg_status_reply status;

if (lkmdbg_session_open(&session) < 0)
    return 1;
if (lkmdbg_session_get_status(session, &status) < 0) {
    lkmdbg_session_close(session);
    return 1;
}
lkmdbg_session_close(session);
```

## Ownership And Concurrency

- `lkmdbg_session_close()` must not race another operation on the same session.
- Independent operations may use one session concurrently; event reads are
  single-consumer unless the application provides its own dispatch layer.
- A remote mapping duplicates the session fd and remains independently
  destructible after the original session object is closed.
- `lkmdbg_remote_map_destroy()` removes the kernel resource before unmapping the
  local view. Target exit and prior removal are accepted during cleanup.
- Failed memory transfers still populate `struct lkmdbg_transfer_result` with
  the kernel-reported partial progress.
