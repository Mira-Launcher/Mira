#pragma once

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;

namespace mira_gui {

// The top of a page of the window (Runners, Downloads): its title, an optional line under it, and
// its own controls on the right, on the page's canvas with the page's margins.
class PageHeader : public QWidget {
  Q_OBJECT

 public:
  explicit PageHeader(const QString& title, QWidget* parent = nullptr);

  // The line under the title ("2 waiting"); hidden while empty.
  void SetSubtitle(const QString& text);
  // A control at the right end, after the ones added before it, kept to the top.
  void AddControl(QWidget* control);

 private:
  QHBoxLayout* layout_ = nullptr;
  QLabel* subtitle_ = nullptr;
};

}  // namespace mira_gui
