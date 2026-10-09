#pragma once

#include <QTimer>
#include <QWidget>

namespace mira_gui::bigscreen {

// A short message in the top corner over a running game, without taking focus.
class Notice : public QWidget {
  Q_OBJECT

public:
  explicit Notice(QWidget* screen_of);

  void Show(const QString& title, const QString& detail = {});

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QWidget* screen_of_;
  QString title_;
  QString detail_;
  QTimer hide_;
};

}  // namespace mira_gui::bigscreen
