#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>
#include <vector>

namespace mira_gui {

// One piece of a drawn page: what it shows, and whether it's the thing to press or copy.
struct SketchPart {
  enum class Kind {
    Heading,
    Text,
    Button,
    Field,
    Code,      // monospace, wrapped anywhere (a JSON page, a key)
    Blank,     // an empty page
    Tabs,      // `text` is the tabs, `selected` the open one
    TableRow,  // `text` is the cells
  };
  Kind kind = Kind::Text;
  QStringList text;
  bool marked = false;  // drawn in the accent: the thing to press or copy
  int step = 0;         // a numbered badge on a marked part; 0 for none
  int selected = -1;
};

// What a sign-in page will look like in the browser (or a launcher's own window), so the steps
// beside it have something to point at.
struct Sketch {
  QString address;  // empty: an app window titled `title` instead of a browser
  QString title;
  bool address_marked = false;  // the address is what to copy
  std::vector<SketchPart> parts;
  QString caption;  // a line under the drawing
};

// Draws a Sketch in the theme's colors, as tall as its width needs.
class BrowserSketch : public QWidget {
  Q_OBJECT

 public:
  explicit BrowserSketch(QWidget* parent = nullptr);

  void SetSketch(const Sketch& sketch);
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override;
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  // Lays out the parts for `width`; draws them when `painter` is set. Returns the height used.
  int Layout(int width, class QPainter* painter) const;

  Sketch sketch_;
};

}  // namespace mira_gui
