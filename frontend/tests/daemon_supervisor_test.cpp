#include <doctest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "app/DaemonSupervisor.h"

using namespace mira_gui;

TEST_CASE("ResolveMiradPath prefers a mirad next to the given binary dir") {
  QTemporaryDir dir;
  REQUIRE(dir.isValid());
  QFile mirad(dir.filePath("mirad"));
  REQUIRE(mirad.open(QIODevice::WriteOnly));
  mirad.close();

  CHECK(ResolveMiradPath(dir.path()).toStdString() == (dir.path() + "/mirad").toStdString());
}

TEST_CASE("ResolveMiradPath falls back to PATH lookup when no mirad sits next to the binary") {
  QTemporaryDir dir;
  REQUIRE(dir.isValid());

  CHECK(ResolveMiradPath(dir.path()).toStdString() == "mirad");
}
