#include "CoverArt.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>

#include "../theme/Theme.h"

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

void DrawFitted(QPainter* painter, const QPointF& centre, qreal side, const QPixmap& pixmap) {
  const QSizeF fit = pixmap.deviceIndependentSize().scaled(side, side, Qt::KeepAspectRatio);
  painter->drawPixmap(QRectF(centre - QPointF(fit.width() / 2, fit.height() / 2), fit), pixmap,
                      pixmap.rect());
}

QPixmap PlaceholderCover(const QString& name, const QString& seed, QSize size, qreal dpr,
                         const QPixmap& logo) {
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

  // Biased upward: the bottom of a tile is where the title scrim goes.
  const QRect face(0, 0, size.width(), size.height() - size.height() / 6);
  if (!logo.isNull()) {
    DrawFitted(&painter, QRectF(face).center(), size.width() * 0.45, logo);
    return pixmap;
  }
  QFont font = painter.font();
  font.setPixelSize(qMax(16, size.height() / 4));
  font.setWeight(QFont::DemiBold);
  painter.setFont(font);
  painter.setPen(QColor(255, 255, 255, 190));
  painter.drawText(face, Qt::AlignCenter, CoverInitials(name));

  return pixmap;
}

QPixmap BlurredCover(const QPixmap& cover, QSize size) {
  return cover.scaled(size / 64, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
      .scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
}

QColor DominantColor(const QPixmap& art) {
  const QImage small = art.toImage()
                           .scaled(24, 36, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                           .convertToFormat(QImage::Format_RGB32);
  struct Bucket {
    double vivid = 0, r = 0, g = 0, b = 0;
    int count = 0;
  };
  constexpr int kHues = 12;
  Bucket buckets[kHues];
  Bucket grey;
  const int total = small.width() * small.height();
  for (int y = 0; y < small.height(); ++y) {
    for (int x = 0; x < small.width(); ++x) {
      const QColor pixel = QColor::fromRgb(small.pixel(x, y));
      const double vivid = pixel.hsvSaturationF() * pixel.valueF();
      Bucket& bucket = vivid < 0.12 ? grey : buckets[std::max(0, pixel.hsvHue()) * kHues / 360];
      bucket.vivid += vivid;
      bucket.r += pixel.red();
      bucket.g += pixel.green();
      bucket.b += pixel.blue();
      ++bucket.count;
    }
  }
  const Bucket* best = nullptr;
  double best_score = 0;
  for (const Bucket& bucket : buckets) {
    if (bucket.count == 0) continue;
    const double score =
        bucket.vivid / std::sqrt(double(bucket.count) * total);  // mean vividness x sqrt(share)
    if (score > best_score) best_score = score, best = &bucket;
  }
  if (best == nullptr || best->count * 25 < total)
    best = grey.count > 0 ? &grey : best;  // under 4%: grey art
  if (best == nullptr) return QColor();
  const QColor mean(qRound(best->r / best->count), qRound(best->g / best->count),
                    qRound(best->b / best->count));
  const int saturation =
      best == &grey ? std::min(mean.hsvSaturation(), 40) : std::max(mean.hsvSaturation(), 115);
  return QColor::fromHsv(mean.hsvHue(), saturation, std::clamp(mean.value(), 158, 216));
}

}  // namespace mira_gui
