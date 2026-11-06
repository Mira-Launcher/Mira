// Test binary entry point. Every other file in tests/ just #includes
// <doctest.h> and adds TEST_CASEs to it; this is the one place the
// implementation (and therefore main()) is generated.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>
#include <signal.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "support/TestEnv.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  // A failed run keeps its files to look at until the next run, which clears those of runs that
  // ended.
  std::error_code ec;
  for (const fs::path& root : mira::test::TestsRoots()) {
    for (const auto& entry : fs::directory_iterator(root.parent_path(), ec)) {
      const std::string name = entry.path().filename().string();
      if (!name.starts_with("mira-tests-")) continue;
      const int pid = std::atoi(name.c_str() + 11);
      if (pid > 0 && ::kill(pid, 0) != 0 && errno == ESRCH) fs::remove_all(entry.path(), ec);
    }
  }
  // Whatever a test doesn't isolate itself lands here rather than in the user's own folders.
  const fs::path home = mira::test::TestsRoot() / "home";
  fs::create_directories(home);
  setenv("HOME", home.c_str(), 1);
  setenv("XDG_CONFIG_HOME", (home / ".config").c_str(), 1);
  setenv("XDG_DATA_HOME", (home / ".local/share").c_str(), 1);
  setenv("XDG_STATE_HOME", (home / ".local/state").c_str(), 1);
  setenv("XDG_CACHE_HOME", (home / ".cache").c_str(), 1);

  doctest::Context context(argc, argv);
  const int failed = context.run();
  if (failed == 0 && !context.shouldExit()) {
    for (const fs::path& root : mira::test::TestsRoots()) fs::remove_all(root, ec);
  }
  return failed;
}
