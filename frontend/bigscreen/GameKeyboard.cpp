#include "GameKeyboard.h"

#include <QGuiApplication>
#include <QPainter>
#include <QScreen>

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "../theme/Theme.h"
#include "BigScreenWindow.h"

namespace mira_gui::bigscreen {
namespace {

constexpr int kShift = -1;
constexpr int kClose = -2;

bool Emit(int fd, int type, int code, int value) {
  input_event event{};
  event.type = std::uint16_t(type);
  event.code = std::uint16_t(code);
  event.value = value;
  return ::write(fd, &event, sizeof(event)) == ssize_t(sizeof(event));
}

}  // namespace

GameKeyboard::GameKeyboard(BigScreenWindow* window)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus | Qt::X11BypassWindowManagerHint),
      window_(window) {
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TranslucentBackground);
  setCursor(Qt::BlankCursor);
  const auto letters = [](const char* text, std::initializer_list<int> codes) {
    std::vector<Key> row;
    auto code = codes.begin();
    for (const char* c = text; *c != '\0'; ++c, ++code) row.push_back({QString(QChar(*c)), *code});
    return row;
  };
  rows_ = {
      letters("1234567890", {KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0}),
      letters("qwertyuiop", {KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P}),
      letters("asdfghjkl'", {KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_APOSTROPHE}),
      letters("zxcvbnm,.-", {KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_MINUS}),
      {{"Shift", kShift, 2}, {"Space", KEY_SPACE, 4}, {"⌫", KEY_BACKSPACE, 1.5}, {"Enter", KEY_ENTER, 1.5},
       {"Close", kClose, 1}},
  };
}

GameKeyboard::~GameKeyboard() {
  if (fd_ >= 0) {
    ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
  }
}

bool GameKeyboard::EnsureDevice(QString* error) {
  if (fd_ >= 0) return true;
  const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    *error = "Typing into games needs access to /dev/uinput.";
    return false;
  }
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_SYN);
  for (const auto& row : rows_) {
    for (const Key& key : row) {
      if (key.code > 0) ioctl(fd, UI_SET_KEYBIT, key.code);
    }
  }
  // Also what app controls and the MangoHud toggle send.
  for (const int code : {KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ESC, KEY_F12}) {
    ioctl(fd, UI_SET_KEYBIT, code);
  }
  uinput_setup setup{};
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0x1209;
  setup.id.product = 0x4d49;
  std::strncpy(setup.name, "Mira on-screen keyboard", UINPUT_MAX_NAME_SIZE - 1);
  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    ::close(fd);
    *error = "Couldn't create a virtual keyboard.";
    return false;
  }
  fd_ = fd;
  return true;
}

bool GameKeyboard::Open(QString* error) {
  if (!EnsureDevice(error)) return false;
  // Along the bottom of the screen the game is on.
  const QRect screen = window_->screen()->geometry();
  const int w = std::min(screen.width(), qRound(screen.height() * 1.25));
  const int h = qRound(screen.height() * 0.36);
  setGeometry(screen.x() + (screen.width() - w) / 2, screen.bottom() - h, w, h);
  show();
  raise();
  return true;
}

bool GameKeyboard::Press(int code, QString* error, int modifier) {
  if (!EnsureDevice(error)) return false;
  if (modifier != 0) Emit(fd_, EV_KEY, modifier, 1);
  Type(code);
  if (modifier != 0) {
    Emit(fd_, EV_KEY, modifier, 0);
    Emit(fd_, EV_SYN, SYN_REPORT, 0);
  }
  return true;
}

void GameKeyboard::Type(int code) {
  if (shift_) Emit(fd_, EV_KEY, KEY_LEFTSHIFT, 1);
  Emit(fd_, EV_KEY, code, 1);
  Emit(fd_, EV_SYN, SYN_REPORT, 0);
  Emit(fd_, EV_KEY, code, 0);
  if (shift_) Emit(fd_, EV_KEY, KEY_LEFTSHIFT, 0);
  Emit(fd_, EV_SYN, SYN_REPORT, 0);
}

void GameKeyboard::Navigate(Nav nav) {
  const auto& row = rows_[size_t(row_)];
  switch (nav) {
    case Nav::Left: column_ = (column_ + int(row.size()) - 1) % int(row.size()); break;
    case Nav::Right: column_ = (column_ + 1) % int(row.size()); break;
    case Nav::Up:
    case Nav::Down: {
      // Keep roughly the same horizontal spot between rows of different widths.
      double x = 0;
      for (int i = 0; i < column_; ++i) x += row[size_t(i)].width;
      x += row[size_t(column_)].width / 2;
      row_ = std::clamp(row_ + (nav == Nav::Down ? 1 : -1), 0, int(rows_.size()) - 1);
      double at = 0;
      column_ = 0;
      for (const Key& key : rows_[size_t(row_)]) {
        if (at + key.width > x) break;
        at += key.width;
        ++column_;
      }
      column_ = std::min(column_, int(rows_[size_t(row_)].size()) - 1);
      break;
    }
    case Nav::Accept: {
      const Key& key = row[size_t(column_)];
      if (key.code == kClose) return hide();
      if (key.code == kShift) {
        shift_ = !shift_;
        break;
      }
      Type(key.code);
      // One capital at a time, like a phone.
      shift_ = false;
      break;
    }
    case Nav::Action: Type(KEY_BACKSPACE); break;
    case Nav::Search: Type(KEY_SPACE); break;
    case Nav::PrevTab: shift_ = !shift_; break;
    case Nav::NextTab: Type(KEY_ENTER); break;
    case Nav::Back:
    case Nav::Guide: return hide();
    default: return;
  }
  update();
}

void GameKeyboard::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = height() / 18.0;
  QColor back = tokens.window;
  back.setAlpha(235);
  painter.setPen(QPen(tokens.border, 1));
  painter.setBrush(back);
  painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, height()), u, u);
  const double pad = u * 0.9, gap = u * 0.35;
  const double key_h = (height() - pad * 2 - u * 2.2 - gap * (double(rows_.size()) - 1)) / double(rows_.size());
  for (size_t r = 0; r < rows_.size(); ++r) {
    double total = 0;
    for (const Key& key : rows_[r]) total += key.width;
    const double unit_w = (width() - pad * 2 - gap * (double(rows_[r].size()) - 1)) / total;
    double x = pad;
    const double y = pad + double(r) * (key_h + gap);
    for (size_t c = 0; c < rows_[r].size(); ++c) {
      const Key& key = rows_[r][c];
      const QRectF box(x, y, key.width * unit_w, key_h);
      const bool focused = int(r) == row_ && int(c) == column_;
      const bool lit = key.code == kShift && shift_;
      painter.setPen(focused ? QPen(Accent(), u * 0.18) : Qt::NoPen);
      painter.setBrush(focused ? tokens.surface_alt : lit ? Accent() : tokens.surface);
      painter.drawRoundedRect(box, u * 0.4, u * 0.4);
      painter.setPen(tokens.text);
      painter.setFont(Font(u, key.label.size() > 1 ? 1.0 : 1.4, QFont::DemiBold));
      painter.drawText(box, Qt::AlignCenter, shift_ && key.label.size() == 1 ? key.label.toUpper() : key.label);
      x += box.width() + gap;
    }
  }
  // The shortcuts, along the bottom.
  const QString kind = window_->GlyphKind();
  QPointF at(pad, height() - pad - u * 0.7);
  painter.setFont(Font(u, 0.9));
  for (const auto& [nav, label] : std::initializer_list<std::pair<Nav, QString>>{
           {Nav::Action, "Delete"}, {Nav::Search, "Space"}, {Nav::PrevTab, "Shift"}, {Nav::NextTab, "Enter"},
           {Nav::Back, "Close"}}) {
    at.rx() += DrawGlyph(painter, at, u, nav, kind) + u * 0.4;
    painter.setPen(tokens.text_muted);
    painter.setFont(Font(u, 0.9));
    painter.drawText(QPointF(at.x(), at.y() + u * 0.32), label);
    at.rx() += painter.fontMetrics().horizontalAdvance(label) + u * 1.4;
  }
}

}  // namespace mira_gui::bigscreen
