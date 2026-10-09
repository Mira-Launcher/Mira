#pragma once

#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// An on-screen keyboard, store filters and the games that match, installed
// or not.
class SearchPage : public Page {
  Q_OBJECT

public:
  explicit SearchPage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;
  void PrefsChanged() override { Search(); }
  bool Typed(const QString& text) override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  enum class Zone { Field, Keys, Filters, Results };
  void Search();
  // The saved searches shown while the query is empty.
  QStringList Recents() const;
  bool ShowRecents() const;
  // How many entries the results zone focuses: recent searches or results.
  int Entries() const;
  void SaveRecent();

  QString query_;
  QStringList filters_;  // "All", "Installed", then source ids
  std::vector<Item> results_;
  Zone zone_ = Zone::Field;
  int key_ = 0;
  int filter_ = 0;
  int result_ = 0;
};

}  // namespace mira_gui::bigscreen
