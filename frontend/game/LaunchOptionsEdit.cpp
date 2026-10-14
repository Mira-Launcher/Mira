#include "LaunchOptionsEdit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "../sources/Sources.h"
#include "../widgets/PathField.h"
#include "../widgets/PopupDialog.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"

namespace mira_gui {
namespace {

constexpr std::pair<const char*, const char*> kModes[] = {
    {"play", "Starts the game"},
    {"alongside", "Runs beside the game"},
    {"tool", "Opens by itself"},
};

QString ModeText(const std::string& mode) {
  for (const auto& [id, text] : kModes) {
    if (mode == id) return text;
  }
  return QString::fromStdString(mode);
}

// An id from the name that no other option has; "game" names the game's own program, and mod managers' ids
// mark them as on the game.
std::string FreeId(const QString& name, const std::vector<LaunchOption>& options) {
  std::string base;
  for (const QChar ch : name.toLower()) {
    if (ch.isLetterOrNumber() && ch.unicode() < 128) base += char(ch.unicode());
    else if (!base.empty() && base.back() != '-') base += '-';
  }
  while (!base.empty() && base.back() == '-') base.pop_back();
  if (base.empty()) base = "option";
  std::set<std::string> taken = {"game"};
  for (const SourceInfo& source : AllSources()) {
    if (source.kind == SourceInfo::Kind::ModManager) taken.insert(source.id.toStdString());
  }
  for (const LaunchOption& option : options) taken.insert(option.id);
  std::string id = base;
  for (int n = 2; taken.contains(id); ++n) id = base + "-" + std::to_string(n);
  return id;
}

}  // namespace

LaunchOptionsEdit::LaunchOptionsEdit(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* list = new QWidget(this);
  list->setObjectName("list_edit");
  list->setAttribute(Qt::WA_StyledBackground);
  auto* list_layout = new QVBoxLayout(list);
  list_layout->setContentsMargins(1, 1, 1, 1);
  list_layout->setSpacing(0);
  rows_ = new QVBoxLayout();
  rows_->setSpacing(0);
  list_layout->addLayout(rows_);
  auto* empty = new QLabel("None", list);
  empty->setProperty("role", "subtle");
  empty->setContentsMargins(10, 6, 10, 6);
  empty_ = empty;
  list_layout->addWidget(empty_);
  auto* add = new QPushButton("Add launch option", list);
  icons::Follow(add, icons::Glyph::Plus, &theme::Tokens::accent);
  add->setObjectName("list_add");
  add->setCursor(Qt::PointingHandCursor);
  connect(add, &QPushButton::clicked, this, [this] {
    LaunchOption option;
    if (!Edit(option)) return;
    option.id = FreeId(QString::fromStdString(option.name), launch_.options);
    launch_.options.push_back(std::move(option));
    Rebuild();
    emit Changed();
  });
  list_layout->addWidget(add, 0, Qt::AlignLeft);
  layout->addWidget(list);

  auto* default_line = new QHBoxLayout();
  default_line->setSpacing(8);
  default_line->addWidget(new QLabel("Play starts", this));
  default_ = new QComboBox(this);
  default_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  default_->setMinimumContentsLength(8);
  default_line->addWidget(default_, /*stretch=*/1);
  layout->addLayout(default_line);
  only_ = new QCheckBox("Offer nothing else", this);
  only_->setToolTip("Ctrl+click Play still shows every choice");
  layout->addWidget(only_);
  connect(default_, &QComboBox::activated, this, [this] {
    launch_.default_id = default_->currentData().toString().toStdString();
    if (launch_.default_id.empty()) launch_.only = false;
    Rebuild();
    emit Changed();
  });
  connect(only_, &QCheckBox::toggled, this, [this](bool on) {
    if (launch_.only == on) return;
    launch_.only = on;
    emit Changed();
  });
  Rebuild();
}

void LaunchOptionsEdit::SetLaunch(const GameLaunch& launch, bool windows, const std::string& install_path) {
  launch_ = launch;
  windows_ = windows;
  install_path_ = install_path;
  Rebuild();
}

GameLaunch LaunchOptionsEdit::Launch() const { return launch_; }

void LaunchOptionsEdit::Rebuild() {
  // deleteLater: a row's own button may be what asked for this.
  while (QLayoutItem* item = rows_->takeAt(0)) {
    if (QWidget* row = item->widget()) {
      row->hide();
      row->deleteLater();
    }
    delete item;
  }
  for (size_t i = 0; i < launch_.options.size(); ++i) {
    const LaunchOption& option = launch_.options[i];
    auto* row = new QWidget(this);
    row->setObjectName("list_row");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 4, 4, 4);
    layout->setSpacing(8);
    auto* text = new QLabel(row);
    text->setTextFormat(Qt::PlainText);
    text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    text->setText(QString::fromStdString(option.name) + "  ·  " + ModeText(option.mode));
    text->setToolTip(QString::fromStdString(option.exe + (option.args.empty() ? "" : " " + option.args)));
    layout->addWidget(text, /*stretch=*/1);
    auto* edit = new QToolButton(row);
    edit->setAutoRaise(true);
    icons::Follow(edit, icons::Glyph::Settings, &theme::Tokens::text_muted);
    edit->setToolTip("Edit");
    connect(edit, &QToolButton::clicked, this, [this, id = option.id] {
      auto find = [this, &id] {
        return std::find_if(launch_.options.begin(), launch_.options.end(),
                            [&](const LaunchOption& o) { return o.id == id; });
      };
      auto it = find();
      if (it == launch_.options.end()) return;
      LaunchOption changed = *it;
      if (!Edit(changed)) return;
      it = find();  // a live reload may have replaced the list while the dialog was open
      if (it == launch_.options.end()) return;
      *it = std::move(changed);
      Rebuild();
      emit Changed();
    });
    layout->addWidget(edit);
    auto* remove = new QToolButton(row);
    remove->setAutoRaise(true);
    icons::Follow(remove, icons::Glyph::Close, &theme::Tokens::text_muted);
    remove->setToolTip("Remove");
    connect(remove, &QToolButton::clicked, this, [this, id = option.id] {
      auto it = std::find_if(launch_.options.begin(), launch_.options.end(),
                             [&](const LaunchOption& o) { return o.id == id; });
      if (it == launch_.options.end()) return;
      if (it->id == launch_.default_id) {
        launch_.default_id.clear();
        launch_.only = false;
      }
      launch_.options.erase(it);
      Rebuild();
      emit Changed();
    });
    layout->addWidget(remove);
    rows_->addWidget(row);
  }
  empty_->setVisible(launch_.options.empty());

  default_->clear();
  default_->addItem("The game's own program", QString());
  for (const LaunchOption& option : launch_.options) {
    if (option.mode == "play") default_->addItem(QString::fromStdString(option.name), QString::fromStdString(option.id));
  }
  const int at = default_->findData(QString::fromStdString(launch_.default_id));
  default_->setCurrentIndex(std::max(at, 0));
  default_->setEnabled(default_->count() > 1);
  only_->setEnabled(at > 0);
  const QSignalBlocker quiet(only_);
  only_->setChecked(at > 0 && launch_.only);
}

bool LaunchOptionsEdit::Edit(LaunchOption& option) {
  PopupDialog dialog(this, notify::Level::Info, option.name.empty() ? "Add launch option" : "Edit launch option");
  dialog.SetMessage("Another program that starts the game (a mod launcher), runs beside it (a deck tracker), or "
                    "opens by itself (a mod manager). {game_dir}, {game_exe} and {prefix} in its arguments are "
                    "filled in.");
  dialog.SetWidth(600);
  auto* form_widget = new QWidget(&dialog);
  auto* form = new QFormLayout(form_widget);
  form->setContentsMargins(0, 0, 0, 0);
  auto* name = new QLineEdit(QString::fromStdString(option.name), form_widget);
  form->addRow("Name", name);
  auto* mode = new QComboBox(form_widget);
  for (const auto& [id, text] : kModes) mode->addItem(text, QString(id));
  mode->setCurrentIndex(std::max(mode->findData(QString::fromStdString(option.mode)), 0));
  form->addRow("What it does", mode);
  auto* exe = new QLineEdit(QString::fromStdString(option.exe), form_widget);
  exe->setPlaceholderText("Absolute, or relative to the install folder");
  form->addRow("Program", PathRow(exe, [this, form_widget] {
                 const QString picked = QFileDialog::getOpenFileName(form_widget, "Choose the program",
                                                                     QString::fromStdString(install_path_));
                 return picked.isEmpty() ? picked : RelativeIfInside(picked, install_path_);
               }));
  auto* args = new QLineEdit(QString::fromStdString(option.args), form_widget);
  args->setPlaceholderText("None");
  form->addRow("Arguments", args);
  auto* native = new QCheckBox("A Linux program, not run in the game's prefix", form_widget);
  native->setChecked(option.native);
  native->setVisible(windows_);
  form->addRow(QString(), native);
  dialog.AddWidget(form_widget);
  dialog.AddButton("Cancel", false);
  QPushButton* save = dialog.AddButton(option.name.empty() ? "Add" : "Save", true, true);
  const auto update = [=] { save->setEnabled(!name->text().trimmed().isEmpty() && !exe->text().trimmed().isEmpty()); };
  connect(name, &QLineEdit::textChanged, save, update);
  connect(exe, &QLineEdit::textChanged, save, update);
  update();
  if (dialog.exec() != QDialog::Accepted) return false;
  option.name = name->text().trimmed().toStdString();
  option.mode = mode->currentData().toString().toStdString();
  option.exe = exe->text().trimmed().toStdString();
  option.args = args->text().trimmed().toStdString();
  option.native = windows_ && native->isChecked();
  if (option.mode != "play" && !option.id.empty() && option.id == launch_.default_id) {
    launch_.default_id.clear();
    launch_.only = false;
  }
  return true;
}

}  // namespace mira_gui
