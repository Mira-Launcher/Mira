#include "SettingEditor.h"

#include <cmath>

#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QUrl>

#include "../client/JsonMapping.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/ListEdit.h"
#include "SettingsCard.h"

namespace mira_gui {

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
  } else if (entry.minimum && entry.maximum && (entry.type == "an integer" || entry.type == "a number")) {
    spin = new QDoubleSpinBox(row);
    spin->setDecimals(entry.type == "an integer" ? 0 : 2);
    spin->setRange(*entry.minimum, *entry.maximum);
    spin->setKeyboardTracking(false);
    spin->setToolTip(QString("Between %1 and %2").arg(*entry.minimum).arg(*entry.maximum));
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
  } else if (entry.type == "an array of strings") {
    const bool folders = entry.path == "folder";
    list = new ListEdit(folders ? ListEdit::Kind::Folder : ListEdit::Kind::Text, folders ? "Add folder…" : "Add…",
                        row);
    row->SetBelow(list);
  } else {
    line = new QLineEdit(row);
    // One width for every field, so a page's fields line up.
    line->setFixedWidth(entry.path.empty() ? 280 : 240);
    // An empty path means Mira finds it (the doc says where), not "nothing".
    if (!entry.path.empty() && entry.default_display.empty()) line->setPlaceholderText("Automatic");
    row->AddControl(line);
    if (entry.is_secret) {
      line->setEchoMode(QLineEdit::Password);
      line->setPlaceholderText("Not set");
      // The eye sits inside the field, so a key field is as wide as any other.
      QAction* show = line->addAction(QIcon(), QLineEdit::TrailingPosition);
      icons::Follow(show, icons::Glyph::Eye, &theme::Tokens::text_muted);
      show->setCheckable(true);
      show->setToolTip("Show");
      QObject::connect(show, &QAction::toggled, line, [line = line, show](bool shown) {
        line->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
        show->setToolTip(shown ? "Hide" : "Show");
      });
    }
    if (!entry.path.empty()) {
      auto* browse = new QPushButton("Browse…", row);
      const bool folder = entry.path == "folder";
      QObject::connect(browse, &QPushButton::clicked, line, [line = line, folder] {
        const QString home = QDir::homePath();
        QString start = line->text().isEmpty() ? home : line->text();
        if (start.startsWith("~")) start = home + start.mid(1);
        const QString chosen = folder ? QFileDialog::getExistingDirectory(line, "Choose a folder", start)
                                      : QFileDialog::getOpenFileName(line, "Choose a file", start);
        if (chosen.isEmpty()) return;
        line->setText(chosen.startsWith(home + "/") ? "~" + chosen.mid(home.size()) : chosen);
      });
      row->AddControl(browse);
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
  return QString::fromStdString(entry.key + ' ' + entry.label + ' ' + entry.category + ' ' + entry.group_label +
                                ' ' + entry.doc + ' ' + entry.keywords);
}

QWidget* SettingEditor::Input() const {
  if (toggle != nullptr) return toggle;
  if (combo != nullptr) return combo;
  if (spin != nullptr) return spin;
  if (list != nullptr) return list;
  return line;
}

std::string SettingEditor::Text() const {
  if (toggle) return toggle->isChecked() ? "true" : "false";
  if (spin) return spin->cleanText().toStdString();
  if (combo) return (combo->isEditable() ? RunnerRef(combo) : combo->currentText()).toStdString();
  if (list) {
    std::vector<std::string> items;
    for (const QString& item : list->Items()) items.push_back(item.toStdString());
    return mapping::ListText(items);
  }
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
  if (list) {
    QStringList items;
    for (const std::string& item : mapping::ParseListText(text)) items << QString::fromStdString(item);
    list->SetItems(items);
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
  if (list) QObject::connect(list, &ListEdit::Changed, context, edited);
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

void FillRunnerCombo(QComboBox* combo, const RunnersResult& runners) {
  const QString current = RunnerRef(combo);
  combo->blockSignals(true);
  combo->clear();
  combo->addItem("Auto (best available)", "auto");
  if (runners.ok) {
    for (const RunnerInfo& runner : runners.runners) {
      const QString label =
          QString("%1 (%2)").arg(QString::fromStdString(runner.name), QString::fromStdString(runner.kind));
      combo->addItem(label, QString::fromStdString(runner.reference));
    }
  }
  ShowRunnerRef(combo, current);
  combo->blockSignals(false);
}

}  // namespace mira_gui
