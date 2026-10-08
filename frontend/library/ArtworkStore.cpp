#include "ArtworkStore.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <optional>

#include "../client/Async.h"
#include "../client/api/Artwork.h"
#include "CoverArt.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

const QSize kMaxArt(800, 1200);
// Big enough for the sidebar's small covers and a dominant color, small enough to keep for every
// game.
const QSize kThumbArt(128, 128);

struct Decoded {
  QImage image;
  QImage thumb;
};

// Real artwork isn't always 2:3 like the tile, so it's scaled to cover and
// centre-cropped rather than letterboxed: a cropped edge reads as a cover,
// a background band reads as a broken image.
//
// Rounded here to the live radius_tile, not a fixed constant: GameTileDelegate
// re-clips the grid's own copy to the same token on every paint, but a plain
// QLabel (the sidebar's cover) has no such second clip, so an unrounded or
// wrongly-rounded pixmap here would show through as-is.
QPixmap FitToTile(const QPixmap& source, QSize tile, qreal device_pixel_ratio, bool quick = false) {
  const QSize target = tile * device_pixel_ratio;
  const QPixmap filled = source.scaled(target, Qt::KeepAspectRatioByExpanding,
                                       quick ? Qt::FastTransformation : Qt::SmoothTransformation);

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

void ArtworkStore::SetQuickScaling(bool quick) {
  const bool ended = quick_ && !quick;
  quick_ = quick;
  if (ended) emit QuickScalingEnded();
}

bool ArtworkStore::Drop(const QString& key) {
  full_.remove(key);
  wanted_.remove(key);
  return thumbs_.remove(key) > 0;
}

QPixmap ArtworkStore::Held(const QString& key) const {
  if (const auto full = full_.constFind(key); full != full_.constEnd()) return *full;
  return thumbs_.value(key);
}

void ArtworkStore::Keep(const QSet<QString>& ids) {
  if (ids == kept_) return;
  kept_ = ids;
  for (auto it = full_.begin(); it != full_.end();) {
    if (kept_.contains(it.key().section('#', 0, 0))) {
      ++it;
    } else {
      it = full_.erase(it);
    }
  }
  // Newly kept: their full images, fetched again where only the small copy is held.
  for (const QString& id : kept_) {
    for (const QString& key : {id, SlotKey(id, kSlots[0])}) {
      if (thumbs_.contains(key) && !full_.contains(key)) {
        refetching_.insert(key);
        Request(key);
      }
    }
  }
}

void ArtworkStore::ForgetWidth(int width) {
  const QString suffix = '@' + QString::number(width);
  for (auto it = scaled_.begin(); it != scaled_.end();) {
    if (it.key().endsWith(suffix)) {
      it = scaled_.erase(it);
    } else {
      ++it;
    }
  }
}

QPixmap ArtworkStore::Cover(const GameSummary& game, QSize tile, qreal device_pixel_ratio) {
  return CoverById(QString::fromStdString(game.id), QString::fromStdString(game.name), tile,
                  device_pixel_ratio);
}

QPixmap ArtworkStore::TitleCover(const QString& source, const QString& ref, const QString& title, QSize tile,
                                 qreal device_pixel_ratio) {
  const QString id = source + "-" + ref;
  titles_.insert(id, {source.toStdString(), ref.toStdString()});
  return Draw(id, title, tile, device_pixel_ratio);
}

QPixmap ArtworkStore::CoverById(const QString& id, const QString& name, QSize tile, qreal device_pixel_ratio) {
  library_.insert(id);  // a store title's id once installed: its copies are the library's now
  return Draw(id, name, tile, device_pixel_ratio);
}

QPixmap ArtworkStore::Draw(const QString& id, const QString& name, QSize tile,
                           qreal device_pixel_ratio) {
  const QString scaled_key = id + '@' + QString::number(tile.width());
  if (const auto cached = scaled_.constFind(scaled_key); cached != scaled_.constEnd())
    return *cached;

  if (!answered_.contains(id)) Request(id);

  if (const auto full = full_.constFind(id); full != full_.constEnd()) {
    const QPixmap cover = FitToTile(*full, tile, device_pixel_ratio, quick_);
    if (!quick_) KeepScaled(scaled_key, cover);
    return cover;
  }
  if (const auto thumb = thumbs_.constFind(id); thumb != thumbs_.constEnd()) {
    // Its full image isn't kept: fetched again for this size, the small copy stretched meanwhile.
    if (!quick_) {
      QList<std::pair<QSize, qreal>>& sizes = wanted_[id];
      if (!sizes.contains(std::pair{tile, device_pixel_ratio}))
        sizes.append({tile, device_pixel_ratio});
      if (!queued_.contains(id)) refetching_.insert(id);
      Request(id);
    }
    const auto last = last_drawn_.constFind(id);
    return FitToTile(last != last_drawn_.constEnd() ? *last : *thumb, tile, device_pixel_ratio);
  }
  // Keyed on the id, not the name, so it survives a rename.
  const QPixmap cover =
      PlaceholderCover(name, id, QSize(tile.width() - 10, tile.height() - 10), device_pixel_ratio);
  KeepScaled(scaled_key, cover);
  return cover;
}

void ArtworkStore::KeepScaled(const QString& key, const QPixmap& cover) {
  // A store title's copies stay only while something on screen still holds them (a pixmap is
  // shared, so that costs nothing); one only this store holds is dropped.
  const QString id = key.section('@', 0, 0);
  for (auto it = scaled_.begin(); it != scaled_.end();) {
    const QString owner = it.key().section('@', 0, 0);
    if (owner != id && !library_.contains(owner) && titles_.contains(owner) && it->isDetached()) {
      it = scaled_.erase(it);
    } else {
      ++it;
    }
  }
  scaled_.insert(key, cover);
  if (library_.contains(id)) last_drawn_.insert(id, cover);
}

QColor ArtworkStore::CoverColor(const QString& id) {
  if (const auto cached = colors_.constFind(id); cached != colors_.constEnd()) return *cached;
  if (!answered_.contains(id)) Request(id);
  QColor color;
  if (const auto art = thumbs_.constFind(id); art != thumbs_.constEnd())
    color = DominantColor(*art);
  if (!color.isValid()) color = PlaceholderBase(id);
  colors_.insert(id, color);
  return color;
}

QPixmap ArtworkStore::SlotArt(const std::string& id, const std::string& slot) {
  const QString key = SlotKey(QString::fromStdString(id), slot);
  if (QPixmap art = Held(key); !art.isNull()) return art;
  // A record that lists its art and leaves this slot out has none to ask for.
  const auto version = slot_versions_.constFind(key);
  if (!answered_.contains(key) && (version == slot_versions_.constEnd() || !version->isEmpty())) Request(key);
  return QPixmap();
}

bool ArtworkStore::HasArtwork(const std::string& id) const {
  return thumbs_.contains(QString::fromStdString(id));
}

QPixmap ArtworkStore::RawArtwork(const std::string& id) const {
  return Held(QString::fromStdString(id));
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
    if (first && (!answered_.contains(slot_key) || thumbs_.contains(slot_key))) continue;
    // Asked before: drop the old answer, and ask again if there's an image now.
    if (queued_.contains(slot_key)) {
      ask_again_.insert(slot_key);
      continue;
    }
    answered_.remove(slot_key);
    const bool had = Drop(slot_key);
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
    if (Drop(key)) {
      InvalidateRendering(id);
      emit CoverChanged(key);
    }
    return;
  }
  if (queued_.contains(key)) {
    ask_again_.insert(key);  // its answer may be the old image
  } else if (answered_.contains(key) && (!first || !thumbs_.contains(key))) {
    // A first version with an image already in hand is that image.
    answered_.remove(key);
    Request(key);
  }
}

void ArtworkStore::Invalidate(const std::string& id) {
  const QString key = QString::fromStdString(id);
  Drop(key);
  answered_.remove(key);
  InvalidateRendering(id);
  Request(key);
  // The other slots are asked again by whoever draws them next.
  for (const char* slot : kSlots) {
    const QString slot_key = SlotKey(key, slot);
    answered_.remove(slot_key);
    slot_versions_.remove(slot_key);
    if (Drop(slot_key)) emit SlotArtChanged(key);
  }
}

void ArtworkStore::TitleArtworkReady(const std::string& id) {
  const QString key = QString::fromStdString(id);
  if (!titles_.contains(key) || thumbs_.contains(key)) return;
  if (queued_.contains(key)) {
    ask_again_.insert(key);  // its answer may predate the fetch
  } else if (answered_.contains(key)) {
    Invalidate(id);
  }
}

void ArtworkStore::InvalidateRendering(const std::string& id) {
  // Every scaled copy, not just the current tile size.
  const QString prefix = QString::fromStdString(id) + '@';
  for (auto it = scaled_.begin(); it != scaled_.end();) {
    if (it.key().startsWith(prefix)) {
      it = scaled_.erase(it);
    } else {
      ++it;
    }
  }
  last_drawn_.remove(QString::fromStdString(id));
  colors_.remove(QString::fromStdString(id));
}

void ArtworkStore::InvalidateAllRenderings() {
  scaled_.clear();
  last_drawn_.clear();
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
      const ArtworkResult result = title ? api::GetTitleArtworkBlocking(title->first, title->second)
                                         : api::GetArtworkBlocking(game, slot);
      Decoded decoded;
      if (!result.ok ||
          !decoded.image.loadFromData(reinterpret_cast<const uchar*>(result.bytes.data()),
                                      static_cast<int>(result.bytes.size()))) {
        return Decoded();
      }
      // Never drawn bigger than the largest tile on a HiDPI screen; the rest is memory.
      if (decoded.image.width() > kMaxArt.width() || decoded.image.height() > kMaxArt.height()) {
        decoded.image =
            decoded.image.scaled(kMaxArt, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      }
      decoded.thumb =
          decoded.image.scaled(kThumbArt, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      return decoded;
    };
    async::Run<Decoded>(this, std::move(fetch), [this, id, hash](Decoded decoded) {
      --in_flight_;
      queued_.remove(id);
      // Answered covers all three outcomes on purpose: re-asking on every
      // repaint would turn an empty library into a request loop. Only
      // Invalidate reopens the question.
      answered_.insert(id);
      // Read before the image is moved out, which leaves it null.
      const bool found = !decoded.image.isNull();
      // Fetched again only for a size or for the sidebar: the same art, so what's drawn stays.
      const bool same_art = refetching_.remove(id) && !ask_again_.contains(id) && found;
      const QString game = hash < 0 ? id : id.left(hash);
      const bool had = thumbs_.contains(id);
      const QList<std::pair<QSize, qreal>> sizes = wanted_.take(id);
      if (found) {
        const QPixmap full = QPixmap::fromImage(std::move(decoded.image));
        thumbs_.insert(id, QPixmap::fromImage(std::move(decoded.thumb)));
        if (kept_.contains(game)) full_.insert(id, full);
        if (hash < 0) {
          if (!same_art) InvalidateRendering(id.toStdString());
          for (const auto& [tile, dpr] : sizes) {
            KeepScaled(id + '@' + QString::number(tile.width()), FitToTile(full, tile, dpr));
          }
        }
      } else {
        Drop(id);  // a refetch found it gone
      }
      if (had || thumbs_.contains(id)) {
        if (hash >= 0) {
          emit SlotArtChanged(game);
        } else {
          if (!found) InvalidateRendering(id.toStdString());
          emit CoverChanged(id);
        }
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
