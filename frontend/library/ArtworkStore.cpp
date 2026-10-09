#include "ArtworkStore.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <optional>
#include <type_traits>
#include <vector>

#include "../client/Async.h"
#include "../client/api/Artwork.h"
#include "CoverArt.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

// mirad's own cap for a cover, so one is drawn as saved.
const QSize kMaxArt(900, 1350);
// Big enough for the sidebar's small covers and a dominant color, small enough to keep for every
// game.
const QSize kThumbArt(128, 128);
// What the sidebar keeps for its games: a 40 px banner or small cover never needs more.
const QSize kSidebarArt(640, 640);

// Store titles' art held at once; the least recently drawn beyond this is dropped.
constexpr int kMaxTitles = 250;
constexpr int kKeepTitles = 200;

// A store title asked for and not drawn since has scrolled away; its request is dropped.
constexpr qint64 kStaleMs = 500;

struct Decoded {
  QImage image;
  bool reduced = false;  // decoded at the asked sizes only, smaller than the file
  QImage thumb;
  QImage sidebar;  // for a game the sidebar shows
  std::vector<QImage> fitted;  // the sizes asked for when the fetch started, in order
};

// Real artwork isn't always 2:3 like the tile, so it's scaled to cover and
// centre-cropped rather than letterboxed: a cropped edge reads as a cover,
// a background band reads as a broken image.
//
// Rounded here to the live radius_tile, not a fixed constant: GameTileDelegate
// re-clips the grid's own copy to the same token on every paint, but a plain
// QLabel (the sidebar's cover) has no such second clip, so an unrounded or
// wrongly-rounded pixmap here would show through as-is.
//
// A template so a fetch's worker thread can do the same to a QImage, with the radius read beforehand.
template <typename Image>
Image FitAnyToTile(const Image& source, QSize tile, qreal device_pixel_ratio, int radius, bool quick = false) {
  const QSize target = tile * device_pixel_ratio;
  const Image filled = source.scaled(target, Qt::KeepAspectRatioByExpanding,
                                     quick ? Qt::FastTransformation : Qt::SmoothTransformation);

  Image out;
  if constexpr (std::is_same_v<Image, QImage>) {
    out = QImage(target, QImage::Format_ARGB32_Premultiplied);
  } else {
    out = QPixmap(target);
  }
  out.fill(Qt::transparent);
  {
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    if (radius > 0) {
      clip.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(target)), radius * device_pixel_ratio,
                          radius * device_pixel_ratio);
    } else {
      clip.addRect(QRectF(QPointF(0, 0), QSizeF(target)));
    }
    painter.setClipPath(clip);
    const QPoint at((target.width() - filled.width()) / 2, (target.height() - filled.height()) / 2);
    if constexpr (std::is_same_v<Image, QImage>) {
      painter.drawImage(at, filled);
    } else {
      painter.drawPixmap(at, filled);
    }
  }
  out.setDevicePixelRatio(device_pixel_ratio);
  return out;
}

QPixmap FitToTile(const QPixmap& source, QSize tile, qreal device_pixel_ratio, bool quick = false) {
  return FitAnyToTile(source, tile, device_pixel_ratio, theme::Current().radius_tile, quick);
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

ArtworkStore::ArtworkStore(QObject* parent) : QObject(parent) { clock_.start(); }

void ArtworkStore::SetQuickScaling(bool quick) {
  const bool ended = quick_ && !quick;
  quick_ = quick;
  if (ended) emit QuickScalingEnded();
}

bool ArtworkStore::Drop(const QString& key) {
  sidebar_.remove(key);
  wanted_.remove(key);
  return thumbs_.remove(key) > 0;
}

QPixmap ArtworkStore::Held(const QString& key) const {
  if (const auto kept = sidebar_.constFind(key); kept != sidebar_.constEnd()) return *kept;
  return thumbs_.value(key);
}

void ArtworkStore::Keep(const QSet<QString>& ids) {
  if (ids == kept_) return;
  kept_ = ids;
  for (auto it = sidebar_.begin(); it != sidebar_.end();) {
    if (kept_.contains(it.key().section('#', 0, 0))) {
      ++it;
    } else {
      it = sidebar_.erase(it);
    }
  }
  // Newly kept: their sidebar copies, fetched again where only the small copy is held.
  for (const QString& id : kept_) {
    for (const QString& key : {id, SlotKey(id, kSlots[0])}) {
      if (thumbs_.contains(key) && !sidebar_.contains(key)) {
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
  TouchTitle(id);
  if (const auto cached = scaled_.constFind(scaled_key); cached != scaled_.constEnd())
    return *cached;

  if (!answered_.contains(id)) {
    // Drawn at this size as soon as it lands.
    if (!quick_ && !queued_.contains(id)) wanted_[id] = {{tile, device_pixel_ratio}};
    Request(id);
  }

  if (const auto thumb = thumbs_.constFind(id); thumb != thumbs_.constEnd()) {
    // Fetched again for this size, the small copy stretched meanwhile.
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
  scaled_.insert(key, cover);
  const QString id = key.section('@', 0, 0);
  if (library_.contains(id)) last_drawn_.insert(id, cover);
}

void ArtworkStore::TouchTitle(const QString& id) {
  if (!titles_.contains(id) || library_.contains(id)) return;
  title_use_.insert(id, clock_.elapsed());
  if (title_use_.size() <= kMaxTitles || trim_queued_) return;
  // After the paint, so every tile it drew is marked first.
  trim_queued_ = true;
  QTimer::singleShot(0, this, &ArtworkStore::TrimTitles);
}

void ArtworkStore::TrimTitles() {
  trim_queued_ = false;
  // Down to kKeepTitles at once, so this sort runs once per 50 new titles. Anything drawn in the
  // last second is on screen or just was, and stays however many that is.
  const qint64 recent = clock_.elapsed() - 1000;
  std::vector<std::pair<qint64, QString>> by_use;
  by_use.reserve(title_use_.size());
  for (auto it = title_use_.cbegin(); it != title_use_.cend(); ++it) by_use.emplace_back(it.value(), it.key());
  std::ranges::sort(by_use);
  for (const auto& [used, old] : by_use) {
    if (title_use_.size() <= kKeepTitles || used > recent) break;
    if (queued_.contains(old)) continue;
    title_use_.remove(old);
    if (library_.contains(old)) continue;  // installed since: the library's now
    Drop(old);
    InvalidateRendering(old.toStdString());
    answered_.remove(old);
    refetching_.remove(old);
  }
}

void ArtworkStore::PrefetchTitle(const QString& source, const QString& ref, QSize tile, qreal device_pixel_ratio) {
  const QString id = source + "-" + ref;
  titles_.insert(id, {source.toStdString(), ref.toStdString()});
  TouchTitle(id);
  if (quick_ || queued_.contains(id) || scaled_.contains(id + '@' + QString::number(tile.width()))) return;
  const bool held = thumbs_.contains(id);
  if (answered_.contains(id) && !held) return;  // it has none
  wanted_[id] = {{tile, device_pixel_ratio}};
  if (held) refetching_.insert(id);
  Request(id, /*ahead=*/true);
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

void ArtworkStore::Request(const QString& id, bool ahead) {
  if (queued_.contains(id)) return;
  queued_.insert(id);
  (ahead ? ahead_ : pending_).enqueue(id);
  Pump();
}

bool ArtworkStore::NextRequest(QQueue<QString>& queue, QString* id) {
  const qint64 since = clock_.elapsed() - kStaleMs;
  while (!queue.isEmpty()) {
    QString next = queue.dequeue();
    const auto used = title_use_.constFind(next);
    if (used == title_use_.constEnd() || *used >= since) {
      *id = std::move(next);
      return true;
    }
    queued_.remove(next);
    wanted_.remove(next);
    refetching_.remove(next);
    // Still on screen if it is drawn again, which asks again.
    if (!drop_signal_queued_) {
      drop_signal_queued_ = true;
      QTimer::singleShot(0, this, [this] {
        drop_signal_queued_ = false;
        emit RequestsDropped();
      });
    }
  }
  return false;
}

void ArtworkStore::Pump() {
  QString id;
  while (in_flight_ < kMaxInFlight && (NextRequest(pending_, &id) || NextRequest(ahead_, &id))) {
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
    // Fitted to the tile there too: a smooth scale per cover on this thread is a stutter per cover.
    const QList<std::pair<QSize, qreal>> sizes = hash < 0 ? wanted_.value(id) : QList<std::pair<QSize, qreal>>();
    const int radius = theme::Current().radius_tile;
    const bool sidebar = kept_.contains(hash < 0 ? id : id.left(hash));
    auto fetch = [game, slot, title, sizes, radius, sidebar] {
      const ArtworkResult result = title ? api::GetTitleArtworkBlocking(title->first, title->second)
                                         : api::GetArtworkBlocking(game, slot);
      Decoded decoded;
      if (!result.ok) return decoded;
      QByteArray bytes = QByteArray::fromRawData(result.bytes.data(), static_cast<qsizetype>(result.bytes.size()));
      QBuffer buffer(&bytes);
      QImageReader reader(&buffer);
      // Never drawn bigger than the largest tile on a HiDPI screen; a bigger image is decoded
      // straight at that size, which for a JPEG skips most of the work.
      const QSize size = reader.size();
      QSize decode = size.width() > kMaxArt.width() || size.height() > kMaxArt.height()
                         ? size.scaled(kMaxArt, Qt::KeepAspectRatio)
                         : size;
      // Only the tile sizes asked for, when nothing needs the whole image: a JPEG decodes at a fraction
      // of its size for a fraction of the work.
      if (!sidebar && !sizes.isEmpty() && size.isValid()) {
        QSize need;
        for (const auto& [tile, dpr] : sizes)
          need = need.expandedTo(size.scaled(tile * dpr, Qt::KeepAspectRatioByExpanding));
        if (need.width() < decode.width()) {
          decode = need;
          decoded.reduced = true;
        }
      }
      if (decode != size) reader.setScaledSize(decode);
      if (!reader.read(&decoded.image)) return Decoded();
      decoded.thumb =
          decoded.image.scaled(kThumbArt, Qt::KeepAspectRatio, Qt::SmoothTransformation);
      for (const auto& [tile, dpr] : sizes) decoded.fitted.push_back(FitAnyToTile(decoded.image, tile, dpr, radius));
      if (sidebar) {
        const QSize size = decoded.image.size();
        decoded.sidebar = size.width() > kSidebarArt.width() || size.height() > kSidebarArt.height()
                              ? decoded.image.scaled(kSidebarArt, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                              : decoded.image;
      }
      return decoded;
    };
    async::Run<Decoded>(this, std::move(fetch), [this, id, hash, sizes](Decoded decoded) {
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
      const QList<std::pair<QSize, qreal>> wanted = wanted_.take(id);
      QList<std::pair<QSize, qreal>> missing;
      if (found) {
        const QPixmap full = QPixmap::fromImage(std::move(decoded.image));
        thumbs_.insert(id, QPixmap::fromImage(std::move(decoded.thumb)));
        if (kept_.contains(game)) {
          sidebar_.insert(id, decoded.sidebar.isNull() ? full.scaled(kSidebarArt, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                                      : QPixmap::fromImage(std::move(decoded.sidebar)));
        }
        if (hash < 0) {
          if (!same_art) InvalidateRendering(id.toStdString());
          for (const auto& size : wanted) {
            const auto& [tile, dpr] = size;
            const qsizetype fitted = sizes.indexOf(size);
            if (fitted >= 0 && fitted < static_cast<qsizetype>(decoded.fitted.size())) {
              KeepScaled(id + '@' + QString::number(tile.width()), QPixmap::fromImage(std::move(decoded.fitted[fitted])));
            } else if (decoded.reduced) {
              missing.append(size);  // asked for after the fetch began: too small here
            } else {
              KeepScaled(id + '@' + QString::number(tile.width()), FitToTile(full, tile, dpr));
            }
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
      } else if (!missing.isEmpty()) {
        wanted_.insert(id, missing);
        refetching_.insert(id);
        Request(id);
      }
      Pump();
    });
  }
}

}  // namespace mira_gui
