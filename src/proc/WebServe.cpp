#include "proc/WebServe.h"

#include <httplib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <thread>

namespace mira::proc {

int ServeWhileRunning(const std::filesystem::path& dir, int port,
                      const std::vector<std::string>& argv) {
  if (argv.empty()) return 2;
  httplib::Server server;
  server.set_default_headers({{"Cross-Origin-Opener-Policy", "same-origin"},
                              {"Cross-Origin-Embedder-Policy", "credentialless"},
                              {"Cache-Control", "no-cache"}});
  // Godot's and Unity's data files; httplib knows .wasm and the web types.
  server.set_file_extension_and_mimetype_mapping("pck", "application/octet-stream");
  server.set_file_extension_and_mimetype_mapping("data", "application/octet-stream");
  server.set_file_extension_and_mimetype_mapping("unityweb", "application/octet-stream");
  if (!server.set_mount_point("/", dir.string())) {
    std::cerr << "mira-run: " << dir << " isn't a folder to serve\n";
    return 1;
  }
  if (!server.bind_to_port("127.0.0.1", port)) {
    std::cerr << "mira-run: port " << port << " is in use\n";
    return 1;
  }
  std::thread listener([&] { server.listen_after_bind(); });

  std::vector<char*> child_argv;
  for (const std::string& arg : argv) child_argv.push_back(const_cast<char*>(arg.c_str()));
  child_argv.push_back(nullptr);
  const pid_t child = ::fork();
  if (child == 0) {
    ::execvp(child_argv[0], child_argv.data());
    std::cerr << "mira-run: couldn't start " << argv.front() << ": " << std::strerror(errno)
              << "\n";
    ::_exit(127);
  }
  int status = 0;
  if (child > 0) {
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
  }
  server.stop();
  listener.join();
  if (child < 0) return 1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

}  // namespace mira::proc
