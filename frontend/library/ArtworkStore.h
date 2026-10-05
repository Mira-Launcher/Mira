#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QSize>
#include <QString>

#include <optional>
#include <string>

#include "../client/Types.h"

namespace mira_gui {

// Cover art for the library: the real thing when mirad has it, the
// generated placeholder when it doesn't, and one place that knows which is
// which.
//
// The per-game artwork fetch answers 404 for anything never fetched, so a
// library of any size is a burst of requests where most come back empty.
// Three rules keep that from being the frontend's problem:
//
//  - **Ask only for what exists, once.** A game record's `art` (NoteArt)
//    says whether there is a cover and which version; one without isn't
//    asked for, and one is asked again only when its version changes.
//  - **At most `kMaxInFlight` at a time.** Every request is a thread and a
//    socket, and decodes its image there; a 500-game library would
//    otherwise open 500 of both at once.
//  - **Keep the original, scale on demand.** The zoom slider changes tile
//    size constantly; re-decoding or re-fetching per step would be absurd.
//
// Everything here is main-thread only.
class ArtworkStore : public QObject {
  Q_OBJECT

public:
  explicit ArtworkStore(QObject* parent = nullptr);

  // The cover to draw for this game at this tile size. Never empty: a game
  // with no artwork gets its placeholder, so a caller never has to decide
  // what to show instead. The first call for an id also queues the fetch.
  QPixmap Cover(const GameSummary& game, QSize tile, qreal device_pixel_ratio);
  // The same by id and name, for a painter that has only those.
  QPixmap CoverById(const QString& id, const QString& name, QSize tile, qreal device_pixel_ratio);

  // The same for a store title that isn't installed yet. Keyed by the id it
  // gets once installed, "<source>-<ref>", so the cover carries over.
  QPixmap TitleCover(const QString& source, const QString& ref, const QString& title, QSize tile,
                     qreal device_pixel_ratio);

  // One vivid color that stands for this game's cover (its placeholder's,
  // until real artwork arrives), for tinting small things like sidebar rows.
  QColor CoverColor(const QString& id);

  // A game's other art slot ("hero"), unscaled. Null until fetched,
  // or when the game has none; the first call queues the fetch, and
  // SlotArtChanged says when it landed.
  QPixmap SlotArt(const std::string& id, const std::string& slot);

  // True only if real artwork is held for this id. False covers both "asked
  // and there was none" and "not asked yet", which is what a bulk re-fetch
  // wants: neither one has a cover to show.
  bool HasArtwork(const std::string& id) const;

  // The unscaled artwork for this id, for a caller that wants to fit it
  // itself rather than Cover()'s tile-shaped crop. Null until fetched,
  // call EnsureRequested() first.
  QPixmap RawArtwork(const std::string& id) const;

  // Queues the fetch if this id hasn't been asked about yet, without also
  // scaling/caching a Cover() for some tile size nobody asked for.
  void EnsureRequested(const std::string& id);

  // What a game record or art event says about this game's cover (its
  // `art`). No cover means nothing to ask for; a new version is fetched
  // again, with the old image shown until it lands. Unset does nothing.
  void NoteArt(const std::string& id, const std::optional<ArtVersions>& art);

  // Forget everything known about one game's artwork and fetch it again.
  void Invalidate(const std::string& id);

  // mirad fetched a store title's cover. Refetches it if TitleCover has
  // asked for it and got nothing yet; otherwise the first ask gets it.
  void TitleArtworkReady(const std::string& id);

  // Drop the scaled copies only; the originals are still good. For a
  // rename, which changes the placeholder's initials but not the artwork.
  void InvalidateRendering(const std::string& id);

  // The same, for every game at once. For a theme change: a generated
  // placeholder is drawn in the theme's colors (see ui/CoverArt), so all of
  // them are stale even though the fetched artwork is not. Also for a new
  // tile size, so the old size's copies don't pile up.
  void InvalidateAllRenderings();

signals:
  // Real artwork arrived (or was dropped) for this id; whatever is drawing
  // it should ask for the cover again.
  void CoverChanged(const QString& id);
  // The same for a SlotArt slot.
  void SlotArtChanged(const QString& id);

private:
  void Request(const QString& id);
  void Pump();

  static constexpr int kMaxInFlight = 8;

  QHash<QString, QPixmap> original_;  // by id, at whatever size mirad sent
  QHash<QString, QHash<int, QPixmap>> scaled_;  // by id, then tile width
  QHash<QString, QColor> colors_;     // CoverColor's, by id
  // SlotArt's, by "id#slot", which also keys answered_/queued_/pending_.
  QHash<QString, QPixmap> slot_original_;
  QHash<QString, QString> slot_versions_;  // from NoteArt; empty for none
  QSet<QString> answered_;            // asked and heard back, either way
  QSet<QString> queued_;              // in `pending_` or in flight
  QSet<QString> ask_again_;           // in flight when mirad said it has art now
  QHash<QString, QString> versions_;  // cover version by id, from NoteArt; empty for none
  QQueue<QString> pending_;
  QHash<QString, std::pair<std::string, std::string>> titles_;  // id -> {source, ref}
  int in_flight_ = 0;
};

}  // namespace mira_gui
