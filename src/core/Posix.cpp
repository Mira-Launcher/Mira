#include "core/Posix.h"

#include <fcntl.h>
#include <unistd.h>

extern char** environ;

namespace mira::posix {

int Pipe(int fds[2]) {
#ifdef __linux__
  return ::pipe2(fds, O_CLOEXEC);
#else
  if (::pipe(fds) != 0) return -1;
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return 0;
#endif
}

int ExecVpe(const char* file, char* const argv[], char* const envp[]) {
#ifdef __linux__
  return ::execvpe(file, argv, envp);
#else
  // Only ever called in a forked child, so replacing environ touches nothing else.
  environ = const_cast<char**>(envp);
  return ::execvp(file, argv);
#endif
}

}  // namespace mira::posix
