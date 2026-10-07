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
//  - **Keep what's shown often, never a placeholder for art already seen.**
//    A small copy of every image stays, and so do the library covers as the
//    grid draws them. Full images stay only for the games the sidebar shows
//    (Keep); any other size is fetched again from mirad's own cache, with the
//    small copy stretched in its place meanwhile.
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

  // A game's other art slot ("hero"), unscaled for a kept game and the small
  // copy for any other. Null until fetched, or when the game has none; the
  // first call queues the fetch, and SlotArtChanged says when it landed.
  QPixmap SlotArt(const std::string& id, const std::string& slot);

  // True only if real artwork is held for this id. False covers both "asked
  // and there was none" and "not asked yet", which is what a bulk re-fetch
  // wants: neither one has a cover to show.
  bool HasArtwork(const std::string& id) const;

  // The unscaled artwork for this id, for a caller that wants to fit it
  // itself rather than Cover()'s tile-shaped crop: the full image for a kept
  // game, the small copy for any other. Null until fetched, call
  // EnsureRequested() first.
  QPixmap RawArtwork(const std::string& id) const;
  // A copy at most 128 px on a side, kept for every cover held, for drawing it small.
  QPixmap SmallArtwork(const std::string& id) const {
    return thumbs_.value(QString::fromStdString(id));
  }

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
  // them are stale even though the fetched artwork is not.
  void InvalidateAllRenderings();

  // While on (the zoom slider moving), a cover not already scaled is scaled
  // quickly and not kept, so each step costs little. Repaint when it's
  // turned off to get the smooth copies.
  void SetQuickScaling(bool quick) { quick_ = quick; }

  // The games whose full images stay (the sidebar's pinned and recently
  // played); any other's is dropped once its sizes are drawn.
  void Keep(const QSet<QString>& ids);
  // Drops the covers drawn at this tile width, once the grid no longer uses it.
  void ForgetWidth(int width);

 signals:
  // Real artwork arrived (or was dropped) for this id; whatever is drawing
  // it should ask for the cover again.
  void CoverChanged(const QString& id);
  // The same for a SlotArt slot.
  void SlotArtChanged(const QString& id);

private:
  void Request(const QString& id);
  void Pump();
  // Forgets the image held for a key; true if there was one.
  bool Drop(const QString& key);
  // The full image for a kept key, else the small copy, else null.
  QPixmap Held(const QString& key) const;
  // Stores a drawn cover, dropping store titles' copies nothing else holds any more.
  void KeepScaled(const QString& key, const QPixmap& cover);
  // CoverById and TitleCover's shared body.
  QPixmap Draw(const QString& id, const QString& name, QSize tile, qreal device_pixel_ratio);

  static constexpr int kMaxInFlight = 8;

  // Keys are a cover's id or a slot's "id#slot" (which also key answered_/queued_/pending_).
  QHash<QString, QPixmap> full_;    // at up to kMaxArt, for kept_ games only
  QSet<QString> kept_;              // game ids
  QSet<QString> library_;           // ids drawn as library games, whose copies always stay
  QHash<QString, QPixmap> thumbs_;  // a small copy of every image held
  // Covers as drawn, by "id@tile width"; a store title's only while something else holds it.
  QHash<QString, QPixmap> scaled_;
  // Sizes asked for while only the small copy was held, drawn when the full image is back.
  QHash<QString, QList<std::pair<QSize, qreal>>> wanted_;
  QSet<QString> refetching_;         // asked again for a size, not because the art changed
  QHash<QString, QColor> colors_;    // CoverColor's, by id
  QHash<QString, QString> slot_versions_;  // from NoteArt; empty for none
  bool quick_ = false;
  QSet<QString> answered_;            // asked and heard back, either way
  QSet<QString> queued_;              // in `pending_` or in flight
  QSet<QString> ask_again_;           // in flight when mirad said it has art now
  QHash<QString, QString> versions_;  // cover version by id, from NoteArt; empty for none
  QQueue<QString> pending_;
  QHash<QString, std::pair<std::string, std::string>> titles_;  // id -> {source, ref}
  int in_flight_ = 0;
};

}  // namespace mira_gui
