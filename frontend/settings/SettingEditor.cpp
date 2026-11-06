#include "SettingEditor.h"

#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

#include "../client/JsonMapping.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/ChipToggles.h"
#include "../widgets/ListEdit.h"
#include "../widgets/PathField.h"
#include "SettingsCard.h"

namespace mira_gui {
namespace {

void ShowSecret(QLineEdit* line, QAction* clear, bool saved) {
  line->setProperty("secret_saved", saved);
  line->setPlaceholderText(saved ? "Saved" : "Not set");
  clear->setVisible(saved);
}

}  // namespace

SettingRow* SettingEditor::Build(QWidget* parent, const QString& doc) {
  const std::string& label = entry.label.empty() ? entry.key : entry.label;
  row = new SettingRow(QString::fromStdString(label), doc.isEmpty() ? Doc() : doc, parent);

  if (entry.type == "a boolean") {
    toggle = new Switch(row);
    toggle->setAccessibleName(QString::fromStdString(label));
    row->AddControl(toggle);
  } else if (!entry.one_of.empty()) {
    combo = new QComboBox(row);
    for (const std::string& option : entry.one_of) combo->addItem(QString::fromStdString(option));
    combo->setMinimumWidth(200);
    row->AddControl(combo);
  } else if (entry.minimum && entry.maximum &&
             (entry.type == "an integer" || entry.type == "a number")) {
    spin = new QDoubleSpinBox(row);
    spin->setDecimals(entry.type == "an integer" ? 0 : 2);
    spin->setRange(*entry.minimum, *entry.maximum);
    spin->setKeyboardTracking(false);
    // Plain digits: QString::arg(double) turns 102400 into 1.024e+05.
    const auto number = [](double value) {
      return QLocale().toString(value, 'f', QLocale::FloatingPointShortest);
    };
    spin->setToolTip(
        QString("Between %1 and %2").arg(number(*entry.minimum), number(*entry.maximum)));
    spin->setMinimumWidth(110);
    row->AddControl(spin);
  } else if (entry.is_runner_ref) {
    combo = new QComboBox(row);
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setFixedWidth(280);
    // Empty is "use the default runner", which an empty box doesn't say.
    combo->lineEdit()->setPlaceholderText("Default runner");
    row->AddControl(combo);
  } else if (entry.type == "an array of strings" && !entry.choices.empty()) {
    chips = new ChipToggles(row);
    std::vector<ChipToggles::Choice> choices;
    for (const ConfigSchemaEntry::Choice& choice : entry.choices) {
      choices.push_back({QString::fromStdString(choice.value), QString::fromStdString(choice.label),
                         QString::fromStdString(choice.doc), QString::fromStdString(choice.pair),
                         choice.needs_pair});
    }
    chips->SetChoices(std::move(choices));
    row->SetBelow(chips);
  } else if (entry.type == "an array of strings") {
    const bool folders = entry.path == "folder";
    list = new ListEdit(folders ? ListEdit::Kind::Folder : ListEdit::Kind::Text,
                        folders ? "Add folder…" : "Add…", row);
    row->SetBelow(list);
  } else {
    line = new QLineEdit(row);
    // One width for every field, so a page's fields line up.
    line->setFixedWidth(entry.path.empty() ? 280 : 240);
    // An empty path means Mira finds it (the doc says where), not "nothing".
    if (!entry.path.empty() && entry.default_display.empty()) line->setPlaceholderText("Automatic");
    if (entry.is_secret) {
      // mirad never sends a saved key back, so there's nothing to show: typing replaces it.
      line->setEchoMode(QLineEdit::Password);
      clear_secret = line->addAction(QIcon(), QLineEdit::TrailingPosition);
      icons::Follow(clear_secret, icons::Glyph::Close, &theme::Tokens::text_muted);
      clear_secret->setToolTip("Remove the saved key");
      QObject::connect(clear_secret, &QAction::triggered, line,
                       [line = line, clear = clear_secret] {
                         line->clear();
                         ShowSecret(line, clear, false);
                       });
      ShowSecret(line, clear_secret, false);
    }
    if (entry.path.empty()) {
      row->AddControl(line);
    } else {
      const bool folder = entry.path == "folder";
      row->AddControl(PathRow(line, [line = line, folder] {
        const QString home = QDir::homePath();
        QString start = line->text().isEmpty() ? home : line->text();
        if (start.startsWith("~")) start = home + start.mid(1);
        const QString chosen =
            folder ? QFileDialog::getExistingDirectory(line, "Choose a folder", start)
                   : QFileDialog::getOpenFileName(line, "Choose a file", start);
        return chosen.startsWith(home + "/") ? "~" + chosen.mid(home.size()) : chosen;
      }));
    }
  }
  if (!entry.link.empty()) {
    const QUrl url(QString::fromStdString(entry.link));
    auto* link = new QToolButton(row);
    link->setAutoRaise(true);
    icons::Follow(link, icons::Glyph::External, &theme::Tokens::accent);
    link->setToolTip(QString("Open %1").arg(url.host()));
    link->setCursor(Qt::PointingHandCursor);
    QObject::connect(link, &QToolButton::clicked, link, [url] { QDesktopServices::openUrl(url); });
    row->AddControl(link);
  }
  return row;
}

QString SettingEditor::Doc() const {
  QString doc = QString::fromStdString(entry.doc);
  if (entry.per_game && !doc.isEmpty()) doc += " Each game can override this in its own settings.";
  return doc;
}

QString SettingEditor::SearchText() const {
  return QString::fromStdString(entry.key + ' ' + entry.label + ' ' + entry.category + ' ' +
                                entry.group_label + ' ' + entry.doc + ' ' + entry.keywords);
}

QWidget* SettingEditor::Input() const {
  if (toggle != nullptr) return toggle;
  if (combo != nullptr) return combo;
  if (spin != nullptr) return spin;
  if (list != nullptr) return list;
  if (chips != nullptr) return chips;
  return line;
}

std::string SettingEditor::Text() const {
  if (toggle) return toggle->isChecked() ? "true" : "false";
  if (spin) return spin->cleanText().toStdString();
  if (combo) return (combo->isEditable() ? RunnerRef(combo) : combo->currentText()).toStdString();
  if (list || chips) {
    std::vector<std::string> items;
    for (const QString& item : list ? list->Items() : chips->Values())
      items.push_back(item.toStdString());
    return mapping::ListText(items);
  }
  if (clear_secret && line->text().isEmpty() && line->property("secret_saved").toBool())
    return kSecretSaved;
  return line->text().toStdString();
}

bool SettingEditor::IsDefault() const {
  // A number box shows "0.50" for a default of 0.5, so compare values.
  if (spin) {
    const double fallback = QString::fromStdString(entry.default_display).toDouble();
    return std::abs(spin->value() - fallback) < 0.5 * std::pow(10.0, -spin->decimals());
  }
  return Text() == entry.default_display;
}

void SettingEditor::SetText(const std::string& text) {
  if (toggle) {
    toggle->setChecked(text == "true");
    return;
  }
  if (list || chips) {
    QStringList items;
    for (const std::string& item : mapping::ParseListText(text))
      items << QString::fromStdString(item);
    if (list)
      list->SetItems(items);
    else
      chips->SetValues(items);
    return;
  }
  if (spin) {
    spin->setValue(QString::fromStdString(text).toDouble());
    return;
  }
  if (combo != nullptr && !combo->isEditable()) {
    const int index = combo->findText(QString::fromStdString(text));
    if (index >= 0) {
      combo->setCurrentIndex(index);
    } else if (!text.empty()) {
      combo->insertItem(0, QString::fromStdString(text));
      combo->setCurrentIndex(0);
    }
    return;
  }
  if (combo != nullptr) {
    ShowRunnerRef(combo, QString::fromStdString(text));
    return;
  }
  if (clear_secret) {
    const bool saved = text == kSecretSaved;
    ShowSecret(line, clear_secret, saved);
    line->setText(saved ? QString() : QString::fromStdString(text));
    return;
  }
  line->setText(QString::fromStdString(text));
  line->setCursorPosition(0);
}

void SettingEditor::OnEdited(QObject* context, std::function<void()> edited) const {
  if (toggle) QObject::connect(toggle, &Switch::toggled, context, edited);
  if (spin) QObject::connect(spin, &QDoubleSpinBox::valueChanged, context, edited);
  if (combo) {
    QObject::connect(combo, &QComboBox::currentTextChanged, context, edited);
    if (combo->isEditable()) QObject::connect(combo, &QComboBox::editTextChanged, context, edited);
  }
  if (line) QObject::connect(line, &QLineEdit::textChanged, context, edited);
  if (clear_secret) QObject::connect(clear_secret, &QAction::triggered, context, edited);
  if (list) QObject::connect(list, &ListEdit::Changed, context, edited);
  if (chips) QObject::connect(chips, &ChipToggles::Changed, context, edited);
}

void SettingEditor::Embed(SettingEditor& shown) {
  if (line == nullptr || shown.chips == nullptr) return;
  auto* box = new QFrame(row);
  box->setObjectName("script_box");
  auto* layout = new QVBoxLayout(box);
  layout->setContentsMargins(8, 8, 8, 4);
  layout->setSpacing(6);
  layout->addWidget(shown.chips);
  // Out of the row's controls, into the box under the chips, as wide as the box.
  line->setMinimumWidth(0);
  line->setMaximumWidth(QWIDGETSIZE_MAX);
  line->setFrame(false);
  line->setObjectName("script_line");
  line->setPlaceholderText("Commands, or a script's path");
  layout->addWidget(line);
  row->SetBelow(box);
  shown.row->hide();
  row->Include(shown.row);
  QObject::connect(row, &SettingRow::RevertClicked, shown.row, &SettingRow::RevertClicked);
}

void PairChoices(const SettingEditor& first, const SettingEditor& second) {
  ChipToggles* from = first.chips;
  ChipToggles* to = second.chips;
  if (from == nullptr || to == nullptr) return;
  QObject::connect(from, &ChipToggles::Clicked, to, [from, to](const QString& value, bool on) {
    const auto& choices = from->Choices();
    const auto it = std::ranges::find(choices, value, &ChipToggles::Choice::value);
    if (it != choices.end() && !it->pair.isEmpty()) to->SetOn(it->pair, on);
  });
  QObject::connect(from, &ChipToggles::Changed, to,
                   [from, to] { to->SetPairValues(from->Values()); });
  to->SetPairValues(from->Values());
}

void ShowRunnerRef(QComboBox* combo, const QString& ref) {
  const int index = ref.isEmpty() ? -1 : combo->findData(ref);
  combo->setEditText(index >= 0 ? combo->itemText(index) : ref);
  combo->lineEdit()->setCursorPosition(0);
}

QString RunnerRef(const QComboBox* combo) {
  const QString text = combo->currentText();
  const int index = combo->findText(text);
  return index >= 0 && combo->itemData(index).isValid() ? combo->itemData(index).toString() : text;
}

void FillRunnerCombo(QComboBox* combo, const RunnersResult& runners,
                     const ConfigSchemaEntry& entry) {
  const QString current = RunnerRef(combo);
  combo->blockSignals(true);
  combo->clear();
  if (entry.runner_kinds.empty() || entry.default_display == "auto")
    combo->addItem("Auto (best available)", "auto");
  if (runners.ok) {
    for (const RunnerInfo& runner : runners.runners) {
      if (!entry.runner_kinds.empty() && !std::ranges::contains(entry.runner_kinds, runner.kind))
        continue;
      const QString label = QString("%1 (%2)").arg(QString::fromStdString(runner.name),
                                                   QString::fromStdString(runner.kind));
      combo->addItem(label, QString::fromStdString(runner.reference));
    }
  }
  ShowRunnerRef(combo, current);
  combo->blockSignals(false);
}

}  // namespace mira_gui
