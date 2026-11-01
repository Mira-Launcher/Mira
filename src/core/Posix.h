#pragma once

namespace mira::posix {

// pipe2(fds, O_CLOEXEC). Where there's no pipe2 the flag is set just after, so a fork on another
// thread in between can leak the ends into that child.
int Pipe(int fds[2]);

// execvpe: `file` looked up in PATH, run with `envp`. Only returns on failure.
int ExecVpe(const char* file, char* const argv[], char* const envp[]);

}  // namespace mira::posix
