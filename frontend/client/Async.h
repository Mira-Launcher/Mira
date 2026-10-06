#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QThreadPool>

#include <exception>
#include <functional>
#include <string>
#include <thread>
#include <utility>

// How every api:: call gets off the UI thread and back onto it.
namespace mira_gui::async {

// Which worker a request runs on.
enum class Lane {
  // Answers in milliseconds: a bounded pool, so a burst (a library's covers)
  // queues instead of starting a thread per request.
  Quick,
  // Can take seconds to hours (network, imports, moving files): a thread of
  // its own, so it never holds up the quick ones behind it.
  Slow,
};

// Never destroyed on purpose: destroying a pool waits for its threads, and
// quitting shouldn't wait on a request nobody will read.
inline QThreadPool* QuickPool() {
  static QThreadPool* pool = [] {
    auto* created = new QThreadPool();
    created->setMaxThreadCount(8);
    return created;
  }();
  return pool;
}

// Hands `fn` to the main thread, dropping it if the object `guard` watches
// has been destroyed in the meantime.
//
// The delivery target is qApp, deliberately, and not the context object
// itself. QMetaObject::invokeMethod dereferences its context argument on the
// *calling* thread, so handing it a QObject* the main thread may already have
// deleted is a use-after-free before the queued call is ever posted.
template <typename Fn>
void Deliver(const QPointer<QObject>& guard, Fn fn) {
  QObject* app = QCoreApplication::instance();
  if (app == nullptr) return;
  QMetaObject::invokeMethod(
      app,
      [guard, fn = std::move(fn)]() mutable {
        if (guard.isNull()) return;
        fn();
      },
      Qt::QueuedConnection);
}

// Runs `work` off the UI thread and delivers its return value to `callback`
// on the main thread, subject to Deliver's liveness rule above. `Result` is
// deduced from the callback, so a caller writes only the request it
// actually wants to make. An empty callback just runs the request.
template <typename Result, typename Work>
void Run(QObject* context, Work work, std::function<void(Result)> callback, Lane lane = Lane::Quick) {
  QPointer<QObject> guard(context);
  std::function<void()> job = [guard, work = std::move(work), callback = std::move(callback)]() mutable {
    // Its page closed while it waited in the queue: nobody wants the answer.
    if (guard.isNull()) return;
    // An exception escaping this thread would abort the whole app. One from
    // parsing (nlohmann's type errors) becomes the result's error instead.
    Result result{};
    try {
      result = work();
    } catch (const std::exception& e) {
      if constexpr (requires { result.error = std::string(); }) result.error = e.what();
    }
    if (!callback) return;  // fire and forget: nobody waits on the answer
    Deliver(guard, [callback, result = std::move(result)]() mutable { callback(std::move(result)); });
  };
  if (lane == Lane::Slow) {
    std::thread(std::move(job)).detach();
  } else {
    QuickPool()->start(std::move(job));
  }
}

}  // namespace mira_gui::async
