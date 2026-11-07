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

#include "../settings/SettingsCard.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/FlatCombo.h"
#include "../widgets/ListEdit.h"
#include "../widgets/PathField.h"
#include "../widgets/PopupDialog.h"

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

// An id from the name that no other option has; "game" names the game's own program, and mod
// managers' ids mark them as on the game.
std::string FreeId(const QString& name, const std::vector<LaunchOption>& options) {
  std::string base;
  for (const QChar ch : name.toLower()) {
    if (ch.isLetterOrNumber() && ch.unicode() < 128)
      base += char(ch.unicode());
    else if (!base.empty() && base.back() != '-')
      base += '-';
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
  setObjectName("list_edit");
  rows_ = new QVBoxLayout(this);
  rows_->setContentsMargins(0, 0, 0, 0);
  rows_->setSpacing(0);

  empty_ = new QLabel("None", this);
  theme::SetStyleProperty(empty_, "role", "subtle");
  add_ = new QPushButton("Add…", this);
  add_->setCursor(Qt::PointingHandCursor);
  connect(add_, &QPushButton::clicked, this, [this] {
    LaunchOption option;
    if (!Edit(option)) return;
    option.id = FreeId(QString::fromStdString(option.name), launch_.options);
    launch_.options.push_back(std::move(option));
    Rebuild();
    emit Changed();
  });

  default_ = new FlatCombo(this);
  only_ = new Switch(this);
  connect(default_, &QComboBox::activated, this, [this] {
    launch_.default_id = default_->currentData().toString().toStdString();
    if (launch_.default_id.empty()) launch_.only = false;
    Rebuild();
    emit Changed();
  });
  connect(only_, &Switch::toggled, this, [this](bool on) {
    if (launch_.only == on) return;
    launch_.only = on;
    emit Changed();
  });
  Rebuild();
}

void LaunchOptionsEdit::SetLaunch(const GameLaunch& launch, bool windows,
                                  const std::string& install_path, const QString& store) {
  launch_ = launch;
  windows_ = windows;
  install_path_ = install_path;
  store_ = store;
  Rebuild();
}

GameLaunch LaunchOptionsEdit::Launch() const {
  return launch_;
}

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
    // As ListEdit's rows: sunken, past the label of the row above, the buttons ending where the
    // controls of the rows around them do.
    auto* row = new QWidget(this);
    QHBoxLayout* layout = ListItemRow(row);
    auto* text = new QLabel(row);
    text->setTextFormat(Qt::PlainText);
    text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    text->setMinimumHeight(kControlHeight);
    text->setText(QString::fromStdString(option.name) + "  ·  " + ModeText(option.mode));
    text->setToolTip(
        QString::fromStdString(option.exe + (option.args.empty() ? "" : " " + option.args)));
    layout->addWidget(text, /*stretch=*/1);
    auto* edit = new QToolButton(row);
    edit->setAutoRaise(true);
    icons::Follow(edit, icons::Glyph::Settings, &theme::Tokens::text_muted);
    edit->setToolTip("Edit");
    connect(edit, &QToolButton::clicked, this, [this, id = option.id] {
      auto it = std::ranges::find(launch_.options, id, &LaunchOption::id);
      if (it == launch_.options.end()) return;
      LaunchOption changed = *it;
      if (!Edit(changed)) return;
      // A live reload may have replaced the list while the dialog was open.
      it = std::ranges::find(launch_.options, id, &LaunchOption::id);
      if (it == launch_.options.end()) return;
      *it = std::move(changed);
      Rebuild();
      emit Changed();
    });
    layout->addWidget(edit);
    QToolButton* remove = NewRemoveButton(row);
    connect(remove, &QToolButton::clicked, this, [this, id = option.id] {
      auto it = std::ranges::find(launch_.options, id, &LaunchOption::id);
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
  setVisible(!launch_.options.empty());

  default_->clear();
  default_->addItem("The game's own program", QString());
  for (const LaunchOption& option : launch_.options) {
    if (option.mode == "play")
      default_->addItem(QString::fromStdString(option.name), QString::fromStdString(option.id));
  }
  const int at = default_->findData(QString::fromStdString(launch_.default_id));
  default_->setCurrentIndex(std::max(at, 0));
  default_->setEnabled(default_->count() > 1);
  only_->setEnabled(at > 0);
  const QSignalBlocker quiet(only_);
  only_->setChecked(at > 0 && launch_.only);
}

bool LaunchOptionsEdit::Edit(LaunchOption& option) {
  PopupDialog dialog(this, notify::Level::Info,
                     option.name.empty() ? "Add launch option" : "Edit launch option");
  dialog.SetMessage(
      "Another program that starts the game (a mod launcher), runs beside it (a deck tracker), or "
      "opens by itself (a mod manager). {game_dir}, {game_exe} and {prefix} in its arguments are "
      "filled in.");
  dialog.SetWidth(600);
  auto* form_widget = new QWidget(&dialog);
  auto* form = new QFormLayout(form_widget);
  form->setContentsMargins(0, 0, 0, 0);
  auto* name = new QLineEdit(QString::fromStdString(option.name), form_widget);
  form->addRow("Name", name);
  auto* mode = new FlatCombo(form_widget);
  for (const auto& [id, text] : kModes) mode->addItem(text, QString(id));
  mode->setCurrentIndex(std::max(mode->findData(QString::fromStdString(option.mode)), 0));
  form->addRow("What it does", mode);
  auto* exe = new QLineEdit(QString::fromStdString(option.exe), form_widget);
  exe->setPlaceholderText("Absolute, or relative to the install folder");
  form->addRow("Program", PathRow(exe, [this, form_widget] {
                 const QString picked = QFileDialog::getOpenFileName(
                     form_widget, "Choose the program", QString::fromStdString(install_path_));
                 return picked.isEmpty() ? picked : RelativeIfInside(picked, install_path_);
               }));
  auto* args = new QLineEdit(QString::fromStdString(option.args), form_widget);
  args->setPlaceholderText("None");
  form->addRow("Arguments", args);
  auto* native = new QCheckBox("A Linux program, not run in the game's prefix", form_widget);
  native->setChecked(option.native);
  native->setVisible(windows_);
  form->addRow(QString(), native);
  // The sign-in holds a single-use code: a program elsewhere, like a mod manager, can't use it.
  auto* sign_in = new FlatCombo(form_widget);
  sign_in->addItem("If it's in the game's folder", -1);
  sign_in->addItem("Yes", 1);
  sign_in->addItem("No", 0);
  sign_in->setCurrentIndex(sign_in->findData(option.sign_in ? int(*option.sign_in) : -1));
  sign_in->setToolTip("Hands the program " + store_ +
                      "'s sign-in, as the game gets it, so achievements and online play work");
  if (!store_.isEmpty()) form->addRow("Signed in to " + store_, sign_in);
  sign_in->setVisible(!store_.isEmpty());
  const auto play_only = [=] { sign_in->setEnabled(mode->currentData().toString() == "play"); };
  connect(mode, &QComboBox::currentIndexChanged, sign_in, play_only);
  play_only();
  dialog.AddWidget(form_widget);
  dialog.AddButton("Cancel", false);
  QPushButton* save = dialog.AddButton(option.name.empty() ? "Add" : "Save", true, true);
  const auto update = [=] {
    save->setEnabled(!name->text().trimmed().isEmpty() && !exe->text().trimmed().isEmpty());
  };
  connect(name, &QLineEdit::textChanged, save, update);
  connect(exe, &QLineEdit::textChanged, save, update);
  update();
  if (dialog.exec() != QDialog::Accepted) return false;
  option.name = name->text().trimmed().toStdString();
  option.mode = mode->currentData().toString().toStdString();
  option.exe = exe->text().trimmed().toStdString();
  option.args = args->text().trimmed().toStdString();
  option.native = windows_ && native->isChecked();
  if (!store_.isEmpty()) {
    const int choice = sign_in->currentData().toInt();
    option.sign_in = choice < 0 ? std::nullopt : std::optional<bool>(choice == 1);
  }
  if (option.mode != "play" && !option.id.empty() && option.id == launch_.default_id) {
    launch_.default_id.clear();
    launch_.only = false;
  }
  return true;
}

}  // namespace mira_gui
