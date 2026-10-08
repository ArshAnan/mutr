#pragma once

#include <cstddef>
#include <sys/types.h>

namespace mutr {

enum class WriteKind { Ok, Blocked, Error };

struct WriteOutcome {
    WriteKind kind = WriteKind::Error;
    std::size_t n = 0;
};

// Ignore SIGPIPE for this socket. On macOS this is SO_NOSIGPIPE.
// send() also passes MSG_NOSIGNAL where that flag exists.
bool setNoSigPipe(int fd);
bool setNonBlocking(int fd);

// Writes some bytes. len must be > 0.
// Ok: n > 0 bytes were written (may be short).
// Blocked: EAGAIN / EWOULDBLOCK, n is 0, nothing was written.
// Error: hard failure (including EPIPE). EINTR is retried inside.
WriteOutcome writeSome(int fd, const char* data, std::size_t len);

// Blocking write of the full buffer. On EAGAIN, waits until the fd is writable.
// Returns false on a hard error. Does not throw.
bool writeAll(int fd, const char* data, std::size_t len);

}  // namespace mutr
