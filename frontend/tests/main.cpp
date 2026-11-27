#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>

#include <cstdlib>

int main(int argc, char** argv) {
  // The desktop's platform theme (KDE's, for one) would hand every test its colors, fonts and
  // style; the runner has none, so tests see Qt's stock ones here too.
  ::unsetenv("QT_QPA_PLATFORMTHEME");
  ::unsetenv("QT_STYLE_OVERRIDE");
  return doctest::Context(argc, argv).run();
}
