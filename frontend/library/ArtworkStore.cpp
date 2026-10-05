#include "ArtworkStore.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <optional>

#include "../client/Async.h"
#include "../client/MiradClient.h"
#include "CoverArt.h"
#include "Theme.h"

namespace mira_gui {
namespace {

const QSize kMaxArt(800, 1200);

// Real artwork isn't always 2:3 like the tile, so it's scaled to cover and
// centre-cropped rather than letterboxed: a cropped edge reads as a cover,
// a background band reads as a broken image.
//
// Rounded here to the live radius_tile, not a fixed constant: GameTileDelegate
// re-clips the grid's own copy to the same token on every paint, but a plain
// QLabel (the sidebar's cover) has no such second clip, so an unrounded or
// wrongly-rounded pixmap here would show through as-is.
QPixmap FitToTile(const QPixmap& source, QSize tile, qreal device_pixel_ratio) {
  const QSize target = tile * device_pixel_ratio;
  const QPixmap filled =
      source.scaled(target, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

  QPixmap out(target);
  out.fill(Qt::transparent);
  {
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    const int radius = theme::Current().radius_tile;
    QPainterPath clip;
    if (radius > 0) {
      clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(target)), radius * device_pixel_ratio,
                          radius * device_pixel_ratio);
    } else {
      clip.addRect(QRectF(QPointF(0, 0), QSizeF(target)));
    }
    painter.setClipPath(clip);
    painter.drawPixmap((target.width() - filled.width()) / 2,
                       (target.height() - filled.height()) / 2, filled);
  }
  out.setDevicePixelRatio(device_pixel_ratio);
  return out;
}

// The most colorful hue that covers a fair share of the cover, not the
// average: averaging most real covers gives grey or brown. Clamped so it
// reads on both light and dark themes.
QColor DominantColor(const QPixmap& art) {
  const QImage small = art.toImage().scaled(24, 36, Qt::IgnoreAspectRatio, Qt::FastTransformation)
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
    const double score = bucket.vivid / std::sqrt(double(bucket.count) * total);  // mean vividness x sqrt(share)
    if (score > best_score) best_score = score, best = &bucket;
  }
  if (best == nullptr || best->count * 25 < total) best = grey.count > 0 ? &grey : best;  // under 4%: a grey cover
  if (best == nullptr) return QColor();
  const QColor mean(qRound(best->r / best->count), qRound(best->g / best->count), qRound(best->b / best->count));
  const int saturation = best == &grey ? std::min(mean.hsvSaturation(), 40) : std::max(mean.hsvSaturation(), 115);
  return QColor::fromHsv(mean.hsvHue(), saturation, std::clamp(mean.value(), 158, 216));
}

constexpr const char* kSlots[] = {"hero"};

QString SlotKey(const QString& id, const std::string& slot) { return id + "#" + QString::fromStdString(slot); }

}  // namespace

ArtworkStore::ArtworkStore(QObject* parent) : QObject(parent) {}

QPixmap ArtworkStore::Cover(const GameSummary& game, QSize tile, qreal device_pixel_ratio) {
  return CoverById(QString::fromStdString(game.id), QString::fromStdString(game.name), tile,
                  device_pixel_ratio);
}

QPixmap ArtworkStore::TitleCover(const QString& source, const QString& ref, const QString& title, QSize tile,
                                 qreal device_pixel_ratio) {
  const QString id = source + "-" + ref;
  titles_.insert(id, {source.toStdString(), ref.toStdString()});
  return CoverById(id, title, tile, device_pixel_ratio);
}

QPixmap ArtworkStore::CoverById(const QString& id, const QString& name, QSize tile, qreal device_pixel_ratio) {
  QHash<int, QPixmap>& sizes = scaled_[id];
  if (const auto cached = sizes.constFind(tile.width()); cached != sizes.constEnd()) return *cached;

  if (!answered_.contains(id)) Request(id);

  QPixmap cover;
  if (const auto art = original_.constFind(id); art != original_.constEnd()) {
    cover = FitToTile(*art, tile, device_pixel_ratio);
  } else {
    // Keyed on the id, not the name, so it survives a rename.
    cover = PlaceholderCover(name, id,
                             QSize(tile.width() - 10, tile.height() - 10), device_pixel_ratio);
  }
  scaled_[id].insert(tile.width(), cover);
  return cover;
}

QColor ArtworkStore::CoverColor(const QString& id) {
  if (const auto cached = colors_.constFind(id); cached != colors_.constEnd()) return *cached;
  if (!answered_.contains(id)) Request(id);
  QColor color;
  if (const auto art = original_.constFind(id); art != original_.constEnd()) color = DominantColor(*art);
  if (!color.isValid()) color = PlaceholderBase(id);
  colors_.insert(id, color);
  return color;
}

QPixmap ArtworkStore::SlotArt(const std::string& id, const std::string& slot) {
  const QString key = SlotKey(QString::fromStdString(id), slot);
  if (const auto art = slot_original_.constFind(key); art != slot_original_.constEnd()) return *art;
  // A record that lists its art and leaves this slot out has none to ask for.
  const auto version = slot_versions_.constFind(key);
  if (!answered_.contains(key) && (version == slot_versions_.constEnd() || !version->isEmpty())) Request(key);
  return QPixmap();
}

bool ArtworkStore::HasArtwork(const std::string& id) const {
  return original_.contains(QString::fromStdString(id));
}

QPixmap ArtworkStore::RawArtwork(const std::string& id) const {
  return original_.value(QString::fromStdString(id));
}

void ArtworkStore::EnsureRequested(const std::string& id) {
  const QString key = QString::fromStdString(id);
  if (!answered_.contains(key)) Request(key);
}

void ArtworkStore::NoteArt(const std::string& id, const std::optional<ArtVersions>& art) {
  if (!art) return;
  const QString key = QString::fromStdString(id);
  for (const char* slot : kSlots) {
    const QString slot_key = SlotKey(key, slot);
    const auto found = art->find(slot);
    const QString version = found != art->end() ? QString::fromStdString(found->second) : QString();
    const auto known = slot_versions_.constFind(slot_key);
    if (known != slot_versions_.constEnd() && *known == version) continue;
    const bool first = known == slot_versions_.constEnd();
    slot_versions_.insert(slot_key, version);
    // Never asked for: SlotArt asks when wanted. A first version with an image in hand is that image.
    if (first && (!answered_.contains(slot_key) || slot_original_.contains(slot_key))) continue;
    // Asked before: drop the old answer, and ask again if there's an image now.
    if (queued_.contains(slot_key)) {
      ask_again_.insert(slot_key);
      continue;
    }
    answered_.remove(slot_key);
    const bool had = slot_original_.remove(slot_key) > 0;
    if (!version.isEmpty()) {
      Request(slot_key);
    } else if (had) {
      emit SlotArtChanged(key);
    }
  }
  const auto cover = art->find("cover");
  const QString version = cover != art->end() ? QString::fromStdString(cover->second) : QString();
  const auto known = versions_.constFind(key);
  const bool first = known == versions_.constEnd();
  if (!first && *known == version) return;
  versions_.insert(key, version);

  if (version.isEmpty()) {
    answered_.insert(key);
    if (original_.remove(key) > 0) {
      InvalidateRendering(id);
      emit CoverChanged(key);
    }
    return;
  }
  if (queued_.contains(key)) {
    ask_again_.insert(key);  // its answer may be the old image
  } else if (answered_.contains(key) && (!first || !original_.contains(key))) {
    // A first version with an image already in hand is that image.
    answered_.remove(key);
    Request(key);
  }
}

void ArtworkStore::Invalidate(const std::string& id) {
  const QString key = QString::fromStdString(id);
  original_.remove(key);
  answered_.remove(key);
  InvalidateRendering(id);
  Request(key);
  // The other slots are asked again by whoever draws them next.
  for (const char* slot : kSlots) {
    const QString slot_key = SlotKey(key, slot);
    answered_.remove(slot_key);
    slot_versions_.remove(slot_key);
    if (slot_original_.remove(slot_key) > 0) emit SlotArtChanged(key);
  }
}

void ArtworkStore::TitleArtworkReady(const std::string& id) {
  const QString key = QString::fromStdString(id);
  if (!titles_.contains(key) || original_.contains(key)) return;
  if (queued_.contains(key)) {
    ask_again_.insert(key);  // its answer may predate the fetch
  } else if (answered_.contains(key)) {
    Invalidate(id);
  }
}

void ArtworkStore::InvalidateRendering(const std::string& id) {
  // Every scaled copy, not just the current tile size: the zoom slider
  // leaves entries behind at every size it passed through.
  scaled_.remove(QString::fromStdString(id));
  colors_.remove(QString::fromStdString(id));
}

void ArtworkStore::InvalidateAllRenderings() {
  scaled_.clear();
  colors_.clear();  // placeholders' colors follow the theme
}

void ArtworkStore::Request(const QString& id) {
  if (queued_.contains(id)) return;
  queued_.insert(id);
  pending_.enqueue(id);
  Pump();
}

void ArtworkStore::Pump() {
  while (in_flight_ < kMaxInFlight && !pending_.isEmpty()) {
    const QString id = pending_.dequeue();
    ++in_flight_;
    std::optional<std::pair<std::string, std::string>> title;
    if (const auto found = titles_.constFind(id); found != titles_.constEnd()) title = *found;
    // Fetched and decoded off the UI thread: a library's worth of covers
    // decoding here is what made the window stutter while they arrived.
    // Loaded from bytes, not a path: the image lives in mirad's own cache
    // directory, which the frontend has no business knowing.
    // "id#slot" is a SlotArt fetch; a plain id is the cover.
    const qsizetype hash = id.indexOf('#');
    const std::string game = (hash < 0 ? id : id.left(hash)).toStdString();
    const std::string slot = hash < 0 ? std::string("cover") : id.mid(hash + 1).toStdString();
    auto fetch = [game, slot, title] {
      const ArtworkResult result = title ? MiradClient::GetTitleArtworkBlocking(title->first, title->second)
                                         : MiradClient::GetArtworkBlocking(game, slot);
      QImage image;
      if (!result.ok || !image.loadFromData(reinterpret_cast<const uchar*>(result.bytes.data()),
                                            static_cast<int>(result.bytes.size()))) {
        return QImage();
      }
      // Never drawn bigger than the largest tile on a HiDPI screen; the rest is memory.
      if (image.width() > kMaxArt.width() || image.height() > kMaxArt.height()) {
        image = image.scaled(kMaxArt, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      }
      return image;
    };
    async::Run<QImage>(this, std::move(fetch), [this, id, hash](QImage image) {
      --in_flight_;
      queued_.remove(id);
      // Answered covers all three outcomes on purpose: re-asking on every
      // repaint would turn an empty library into a request loop. Only
      // Invalidate reopens the question.
      answered_.insert(id);
      if (hash >= 0) {
        const bool had = slot_original_.contains(id);
        if (!image.isNull()) slot_original_.insert(id, QPixmap::fromImage(std::move(image)));
        else slot_original_.remove(id);
        if (had || slot_original_.contains(id)) emit SlotArtChanged(id.left(hash));
      } else if (!image.isNull()) {
        original_.insert(id, QPixmap::fromImage(std::move(image)));
        InvalidateRendering(id.toStdString());
        emit CoverChanged(id);
      } else if (original_.remove(id) > 0) {
        // A refetch found it gone.
        InvalidateRendering(id.toStdString());
        emit CoverChanged(id);
      }
      if (ask_again_.remove(id)) {
        answered_.remove(id);
        Request(id);
      }
      Pump();
    });
  }
}

}  // namespace mira_gui
