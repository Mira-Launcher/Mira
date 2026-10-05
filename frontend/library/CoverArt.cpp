#include "CoverArt.h"

#include "Theme.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QRegularExpression>
#include <QStringList>

namespace mira_gui {

QColor PlaceholderBase(const QString& seed) {
  const int hue = static_cast<int>(qHash(seed) % 360u);
  const theme::Tokens& tokens = theme::Current();
  return QColor::fromHsv(hue, tokens.placeholder_saturation, tokens.placeholder_value);
}

QString CoverInitials(const QString& name) {
  QString initials;
  const QStringList words = name.split(QRegularExpression("[\\s_\\-:]+"), Qt::SkipEmptyParts);
  for (const QString& word : words) {
    if (word.isEmpty() || !word.at(0).isLetterOrNumber()) continue;
    initials.append(word.at(0).toUpper());
    if (initials.size() == 2) break;
  }
  if (initials.isEmpty() && !name.isEmpty()) initials = name.left(1).toUpper();
  return initials;
}

QPixmap PlaceholderCover(const QString& name, const QString& seed, QSize size, qreal dpr) {
  if (size.isEmpty()) return QPixmap();

  QPixmap pixmap(size * dpr);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);

  const QColor base = PlaceholderBase(seed);
  QLinearGradient gradient(0, 0, size.width(), size.height());
  gradient.setColorAt(0.0, base.lighter(135));
  gradient.setColorAt(1.0, base.darker(150));

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.fillRect(QRect(QPoint(0, 0), size), gradient);

  // A single off-centre band, angled, so two covers with neighbouring hues
  // still read as different tiles at a glance.
  painter.save();
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(255, 255, 255, 16));
  painter.translate(size.width() * 0.5, size.height() * 0.5);
  painter.rotate(-28.0 + static_cast<int>(qHash(seed) % 56u));
  painter.drawRect(QRect(-size.width(), -size.height() / 12, size.width() * 2, size.height() / 6));
  painter.restore();

  QFont font = painter.font();
  font.setPixelSize(qMax(16, size.height() / 4));
  font.setWeight(QFont::DemiBold);
  painter.setFont(font);
  painter.setPen(QColor(255, 255, 255, 190));
  // Biased upward: the bottom of a tile is where the title scrim goes.
  painter.drawText(QRect(0, 0, size.width(), size.height() - size.height() / 6), Qt::AlignCenter,
                   CoverInitials(name));

  return pixmap;
}

}  // namespace mira_gui
