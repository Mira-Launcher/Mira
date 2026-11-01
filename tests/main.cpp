// Test binary entry point. Every other file in tests/ just #includes
// <doctest.h> and adds TEST_CASEs to it; this is the one place the
// implementation (and therefore main()) is generated.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <filesystem>
#include <system_error>

int main(int argc, char** argv) {
  doctest::Context context(argc, argv);
  const int failed = context.run();
  // Every test's files live under $TMPDIR/mira-tests; a failed run keeps them to look at.
  if (failed == 0 && !context.shouldExit()) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "mira-tests", ec);
  }
  return failed;
}
