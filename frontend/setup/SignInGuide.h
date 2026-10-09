#pragma once

#include <QString>
#include <QStringList>
#include <string>

#include "BrowserSketch.h"

namespace mira_gui {

// How to connect one source, in plain words: the steps after opening its page, a drawing of
// what that page will show, and what to say once it worked. Login URLs and what counts as a
// credential come from mirad; this is only the wording.
struct SignInGuide {
  QString title;  // "Sign in to Epic Games"
  QString line;   // the source's row in Set up Mira
  QString open;   // the button that opens the page; empty for a launcher
  QString intro;  // a sentence over the steps, when the source works differently
  QStringList steps;
  Sketch sketch;
  // A second browser's steps and drawing, when they differ (Humble's cookie in Chrome or Edge).
  QStringList other_steps;
  Sketch other_sketch;
  QString note;
  bool note_private = false;  // the note is about keeping something secret
  QString done;               // "Signed in" (an account name is added when mirad knows one)
};

// The guide for a store, a launcher, or "steam" (its owned-games key).
SignInGuide GuideFor(const std::string& id);

}  // namespace mira_gui
