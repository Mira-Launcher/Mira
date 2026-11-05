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
#include <QPushButton>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

#include "../client/JsonMapping.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/ChipToggles.h"
#include "../widgets/FlatCombo.h"
#include "../widgets/Labels.h"
#include "../widgets/ListEdit.h"
#include "../widgets/PathField.h"
#include "../widgets/Segmented.h"
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
    const auto option_label = [this](size_t i) {
      return QString::fromStdString(i < entry.option_labels.size() ? entry.option_labels[i]
                                                                   : entry.one_of[i]);
    };
    if (entry.one_of.size() <= 3 && !entry.option_labels.empty()) {
      segments = new Segmented(row);
      segments->setAccessibleName(QString::fromStdString(label));
      for (size_t i = 0; i < entry.one_of.size(); ++i)
        segments->AddOption(QString::fromStdString(entry.one_of[i]), option_label(i));
      row->AddControl(segments);
    } else {
      combo = new FlatCombo(row);
      for (size_t i = 0; i < entry.one_of.size(); ++i)
        combo->addItem(option_label(i), QString::fromStdString(entry.one_of[i]));
      row->AddControl(combo);
    }
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
    if (!entry.unit.empty()) spin->setSuffix(" " + QString::fromStdString(entry.unit));
    spin->setFixedWidth(kNumberWidth);
    row->AddControl(spin);
  } else if (entry.is_runner_ref) {
    combo = new FlatCombo(row);
    combo->addItem("Default runner", QString());  // the runners come later (FillRunnerCombo)
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
    row->AddControl(list->EmptyLabel());
    row->AddControl(list->AddButton());
    row->SetBelowFlush(list);
  } else {
    line = new QLineEdit(row);
    // An empty path means Mira finds it (the doc says where), not "nothing"; any other empty
    // text (a script, an id) is nothing at all.
    if (entry.default_display.empty())
      line->setPlaceholderText(entry.path.empty() ? "None" : "Automatic");
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
    // The field and what goes with it (Browse…, a link) take one width, so fields line up.
    std::vector<QWidget*> buttons;
    if (!entry.path.empty()) {
      const bool folder = entry.path == "folder";
      auto* browse = new QPushButton("Browse…", row);
      QObject::connect(browse, &QPushButton::clicked, line, [line = line, folder] {
        const QString start = line->text().isEmpty() ? QDir::homePath() : ExpandHome(line->text());
        const QString chosen =
            folder ? QFileDialog::getExistingDirectory(line, "Choose a folder", start)
                   : QFileDialog::getOpenFileName(line, "Choose a file", start);
        if (chosen.isEmpty()) return;
        line->setText(HomeRelative(chosen));
        ShowPathEnd(line);
      });
      buttons.push_back(browse);
    }
    if (!entry.link.empty()) {
      const QUrl url(QString::fromStdString(entry.link));
      auto* link = new QToolButton(row);
      link->setAutoRaise(true);
      icons::Follow(link, icons::Glyph::External, &theme::Tokens::accent);
      link->setToolTip(QString("Open %1").arg(url.host()));
      link->setCursor(Qt::PointingHandCursor);
      QObject::connect(link, &QToolButton::clicked, link,
                       [url] { QDesktopServices::openUrl(url); });
      buttons.push_back(link);
    }
    if (buttons.empty()) {
      line->setFixedWidth(kFieldColumnWidth);
      row->AddControl(line);
    } else {
      row->AddControl(FieldWithButtons(line, buttons));
    }
  }
  return row;
}

QString SettingEditor::Doc() const {
  QString doc = QString::fromStdString(entry.doc);
  if (entry.per_game && !doc.isEmpty()) doc += " Each game can override this in its own settings.";
  return doc;
}

QString SettingEditor::SearchText() const {
  return QString::fromStdString(entry.key + ' ' + entry.label + ' ' + entry.item_label + ' ' +
                                entry.category + ' ' + entry.group_label + ' ' + entry.doc + ' ' +
                                entry.keywords);
}

QWidget* SettingEditor::Input() const {
  if (toggle != nullptr) return toggle;
  if (combo != nullptr) return combo;
  if (segments != nullptr) return segments;
  if (spin != nullptr) return spin;
  if (list != nullptr) return list;
  if (chips != nullptr) return chips;
  return line;
}

std::string SettingEditor::Text() const {
  if (toggle) return toggle->isChecked() ? "true" : "false";
  if (spin) return spin->cleanText().toStdString();
  if (segments) return segments->Value().toStdString();
  if (combo) {
    if (entry.is_runner_ref) return RunnerRef(combo).toStdString();
    const QVariant value = combo->currentData();
    return (value.isValid() ? value.toString() : combo->currentText()).toStdString();
  }
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
  if (segments) {
    segments->SetValue(QString::fromStdString(text));
    return;
  }
  if (combo != nullptr && !entry.is_runner_ref) {
    const int index = combo->findData(QString::fromStdString(text));
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
  if (segments) QObject::connect(segments, &Segmented::Changed, context, edited);
  if (combo) QObject::connect(combo, &QComboBox::currentIndexChanged, context, edited);
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
  int index = combo->findData(ref);
  if (index < 0) {
    // A runner this machine doesn't list (removed, or set by hand), kept as it was set.
    combo->addItem(ref, ref);
    index = combo->count() - 1;
  }
  combo->setCurrentIndex(index);
}

QString RunnerRef(const QComboBox* combo) {
  return combo->currentData().toString();
}

void FillRunnerCombo(QComboBox* combo, const RunnersResult& runners,
                     const ConfigSchemaEntry& entry) {
  const QString current = RunnerRef(combo);
  combo->blockSignals(true);
  combo->clear();
  combo->addItem("Default runner", QString());
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
