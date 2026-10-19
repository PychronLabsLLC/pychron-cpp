#include "question_editor.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include "pychron/setup/template.hpp"

namespace pychron::ui {

using namespace pychron::setup;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return s.toStdString(); }

constexpr int kChoiceRadioLimit = 4;  // more choices than this: a combo box

// A number set_question_value could not put in a spin box, as text.
constexpr const char* kOutOfRange = "pychron_out_of_range";

QLineEdit* line_edit_of(QWidget* editor) {
  if (auto* e = qobject_cast<QLineEdit*>(editor)) return e;
  return editor->findChild<QLineEdit*>(QStringLiteral("text"));
}

const QLineEdit* line_edit_of(const QWidget* editor) {
  if (const auto* e = qobject_cast<const QLineEdit*>(editor)) return e;
  return editor->findChild<const QLineEdit*>(QStringLiteral("text"));
}

// parse_answer's reason without the question id it starts with: the field
// shows it beside the question.
Result<Value> parsed(const Question& q, const std::string& text) {
  auto v = parse_answer(q, text);
  if (v) return v;
  std::string what = v.error().what;
  if (what.starts_with(q.id + ": ")) what = what.substr(q.id.size() + 2);
  return fail(ErrorKind::Config, what);
}

QWidget* make_table(const Question& q) {
  auto* box = new QWidget;
  auto* v = new QVBoxLayout(box);
  v->setContentsMargins(0, 0, 0, 0);
  auto* table = new QTableWidget(0, static_cast<int>(q.columns.size()), box);
  table->setObjectName(QStringLiteral("table"));
  QStringList headers;
  for (const auto& c : q.columns) headers << qs(c);
  table->setHorizontalHeaderLabels(headers);
  table->horizontalHeader()->setStretchLastSection(true);
  table->verticalHeader()->setVisible(false);
  table->setMinimumHeight(180);
  auto* buttons = new QHBoxLayout;
  auto* add = new QPushButton(QObject::tr("Add row"), box);
  auto* remove = new QPushButton(QObject::tr("Remove row"), box);
  buttons->addWidget(add);
  buttons->addWidget(remove);
  buttons->addStretch(1);
  v->addWidget(table);
  v->addLayout(buttons);
  QObject::connect(add, &QPushButton::clicked, table, [table] { table->insertRow(table->rowCount()); });
  QObject::connect(remove, &QPushButton::clicked, table, [table] {
    const int r = table->currentRow();
    table->removeRow(r >= 0 ? r : table->rowCount() - 1);
  });
  return box;
}

QWidget* make_unparented(const Question& q, const std::function<void()>& changed) {
  // Each editor's signal goes to `changed` with the editor as context, so it
  // ends with the editor.
  auto heard = [changed] {
    if (changed) changed();
  };
  switch (q.type) {
    case QuestionType::Bool: {
      auto* box = new QCheckBox(qs(q.prompt));
      QObject::connect(box, &QCheckBox::toggled, box, heard);
      return box;
    }
    case QuestionType::Port:
    case QuestionType::Int: {
      auto* spin = new QSpinBox;
      if (q.type == QuestionType::Port) spin->setRange(1, 65535);
      else spin->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
      spin->setMaximumWidth(140);
      QObject::connect(spin, &QSpinBox::valueChanged, spin, [spin, heard] {
        spin->setProperty(kOutOfRange, QVariant());  // the user chose a number it can hold
        heard();
      });
      return spin;
    }
    case QuestionType::Choice: {
      const bool labelled = q.labels.size() == q.choices.size();
      if (q.choices.size() <= kChoiceRadioLimit) {
        auto* box = new QWidget;
        auto* v = new QVBoxLayout(box);
        v->setContentsMargins(0, 0, 0, 0);
        auto* group = new QButtonGroup(box);
        for (std::size_t i = 0; i < q.choices.size(); ++i) {
          auto* r = new QRadioButton(qs(labelled ? q.labels[i] : q.choices[i]), box);
          r->setObjectName(qs(q.id + "-" + q.choices[i]));
          group->addButton(r, static_cast<int>(i));
          v->addWidget(r);
        }
        // idToggled, not idClicked: a value set from code is heard too.
        QObject::connect(group, &QButtonGroup::idToggled, box, [heard](int, bool on) {
          if (on) heard();
        });
        return box;
      }
      auto* combo = new QComboBox;
      for (std::size_t i = 0; i < q.choices.size(); ++i)
        combo->addItem(qs(labelled ? q.labels[i] : q.choices[i]), qs(q.choices[i]));
      QObject::connect(combo, &QComboBox::currentIndexChanged, combo, heard);
      return combo;
    }
    case QuestionType::Table:
      return make_table(q);
    case QuestionType::Path:
    case QuestionType::Folder: {
      auto* box = new QWidget;
      auto* h = new QHBoxLayout(box);
      h->setContentsMargins(0, 0, 0, 0);
      auto* edit = new QLineEdit(box);
      edit->setObjectName(QStringLiteral("text"));
      auto* browse = new QPushButton(QObject::tr("Browse…"), box);
      browse->setObjectName(QStringLiteral("browse"));
      h->addWidget(edit, 1);
      h->addWidget(browse);
      QObject::connect(edit, &QLineEdit::textChanged, edit, heard);
      const bool folder = q.type == QuestionType::Folder;
      QObject::connect(browse, &QPushButton::clicked, box, [box, edit, folder] {
        const QString f = folder ? QFileDialog::getExistingDirectory(box, QString(), edit->text())
                                 : QFileDialog::getOpenFileName(box, QString(), edit->text());
        if (!f.isEmpty()) edit->setText(f);
      });
      return box;
    }
    case QuestionType::String:
    case QuestionType::Host:
    case QuestionType::Float:
    case QuestionType::Secret:
    case QuestionType::List: {
      auto* edit = new QLineEdit;
      if (q.type == QuestionType::Secret) edit->setEchoMode(QLineEdit::Password);
      if (q.type == QuestionType::List) edit->setPlaceholderText(QObject::tr("comma separated"));
      QObject::connect(edit, &QLineEdit::textChanged, edit, heard);
      return edit;
    }
  }
  return new QWidget;
}

}  // namespace

QWidget* make_question_editor(const Question& q, QWidget* parent, const std::function<void()>& changed) {
  QWidget* editor = make_unparented(q, changed);
  if (parent != nullptr) editor->setParent(parent);
  return editor;
}

void set_question_value(QWidget* editor, const Question& q, const Value& value) {
  if (editor == nullptr) return;
  switch (q.type) {
    case QuestionType::Bool:
      if (auto* b = qobject_cast<QCheckBox*>(editor)) b->setChecked(truthy(value));
      return;
    case QuestionType::Port:
    case QuestionType::Int:
      if (auto* s = qobject_cast<QSpinBox*>(editor)) {
        std::int64_t n = 0;
        if (const auto* i = std::get_if<std::int64_t>(&value)) n = *i;
        else n = QString::fromStdString(to_text(value)).toLongLong();
        if (n < s->minimum() || n > s->maximum()) {
          // Not clamped: a recorded 70000 is not silently a 65535.
          s->setProperty(kOutOfRange, qs(to_text(value)));
        } else {
          s->setValue(static_cast<int>(n));
          s->setProperty(kOutOfRange, QVariant());
        }
      }
      return;
    case QuestionType::Choice: {
      const std::string text = to_text(value);
      const auto it = std::find(q.choices.begin(), q.choices.end(), text);
      if (it == q.choices.end()) return;
      const int index = static_cast<int>(it - q.choices.begin());
      if (auto* combo = qobject_cast<QComboBox*>(editor)) {
        combo->setCurrentIndex(index);
      } else if (auto* group = editor->findChild<QButtonGroup*>()) {
        if (auto* b = group->button(index)) b->setChecked(true);
      }
      return;
    }
    case QuestionType::Table: {
      auto* table = editor->findChild<QTableWidget*>(QStringLiteral("table"));
      const auto* rows = std::get_if<std::vector<Row>>(&value);
      if (table == nullptr || rows == nullptr) return;
      table->setRowCount(static_cast<int>(rows->size()));
      for (std::size_t r = 0; r < rows->size(); ++r) {
        for (std::size_t c = 0; c < q.columns.size(); ++c) {
          auto cell = (*rows)[r].find(q.columns[c]);
          table->setItem(static_cast<int>(r), static_cast<int>(c),
                         new QTableWidgetItem(cell == (*rows)[r].end() ? QString() : qs(cell->second)));
        }
      }
      return;
    }
    default:
      if (auto* e = line_edit_of(editor)) {
        if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
          QStringList parts;
          for (const auto& s : *list) parts << qs(s);
          e->setText(parts.join(QStringLiteral(", ")));
        } else {
          e->setText(qs(to_text(value)));
        }
      }
      return;
  }
}

Result<Value> question_value(const QWidget* editor, const Question& q) {
  if (editor == nullptr) return fail(ErrorKind::Config, "no editor");
  switch (q.type) {
    case QuestionType::Bool: {
      const auto* box = qobject_cast<const QCheckBox*>(editor);
      return Value{box != nullptr && box->isChecked()};
    }
    case QuestionType::Port:
    case QuestionType::Int: {
      const auto* spin = qobject_cast<const QSpinBox*>(editor);
      if (spin == nullptr) return fail(ErrorKind::Config, "no number");
      if (const QVariant held = spin->property(kOutOfRange); held.isValid()) return parsed(q, ss(held.toString()));
      return Value{static_cast<std::int64_t>(spin->value())};
    }
    case QuestionType::Choice: {
      int index = -1;
      if (const auto* combo = qobject_cast<const QComboBox*>(editor)) index = combo->currentIndex();
      else if (const auto* group = editor->findChild<const QButtonGroup*>()) index = group->checkedId();
      if (index < 0 || std::cmp_greater_equal(index, q.choices.size())) return fail(ErrorKind::Config, "choose one");
      return Value{q.choices[static_cast<std::size_t>(index)]};
    }
    case QuestionType::Table: {
      const auto* table = editor->findChild<const QTableWidget*>(QStringLiteral("table"));
      std::vector<Row> rows;
      if (table == nullptr) return Value{std::move(rows)};
      for (int r = 0; r < table->rowCount(); ++r) {
        Row row;
        bool any = false;
        for (int c = 0; c < table->columnCount(); ++c) {
          const QTableWidgetItem* item = table->item(r, c);
          const std::string text = item ? ss(item->text().trimmed()) : std::string{};
          any = any || !text.empty();
          row[q.columns[static_cast<std::size_t>(c)]] = text;
        }
        if (any) rows.push_back(std::move(row));
      }
      return Value{std::move(rows)};
    }
    default: {
      const QLineEdit* e = line_edit_of(editor);
      if (q.type == QuestionType::Secret) return Value{ss(e ? e->text() : QString())};
      return parsed(q, ss(e ? e->text().trimmed() : QString()));
    }
  }
}

}  // namespace pychron::ui
