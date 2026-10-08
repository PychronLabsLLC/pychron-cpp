#include "options_editor.hpp"
#include "flow_layout.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString number_text(double v) { return QString::number(v, 'g', 12); }

void mark(QWidget* w, const QString& error) {
  style::set_invalid(w, !error.isEmpty());
  w->setToolTip(error.isEmpty() ? w->property("help").toString() : error);
}

QString swatch_style(const QString& color) {
  if (color.isEmpty()) return QStringLiteral("QPushButton { text-align: left; }");
  return QStringLiteral("QPushButton { background: %1; color: %2; text-align: left; }")
      .arg(color, QColor(color).lightness() < 128 ? QStringLiteral("white") : QStringLiteral("black"));
}

// The families a figure can be drawn in, without the system's private ones.
const QStringList& font_families() {
  static const QStringList families = [] {
    QStringList out;
    for (const QString& family : QFontDatabase::families())
      if (!QFontDatabase::isPrivateFamily(family)) out << family;
    return out;
  }();
  return families;
}

// "key == value", "key != value", "key in v1|v2|..." or "key" (a bool).
bool condition_holds(const std::string& cond, const pp::Options& o) {
  if (cond.empty()) return true;
  auto trim = [](std::string s) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
  };
  for (const char* op : {"==", "!="}) {
    const auto p = cond.find(op);
    if (p == std::string::npos) continue;
    const std::string key = trim(cond.substr(0, p)), value = trim(cond.substr(p + 2));
    if (!o.schema() || !o.schema()->field(key)) return true;
    const bool eq = o.get_string(key) == value;
    return op[0] == '=' ? eq : !eq;
  }
  if (const auto p = cond.find(" in "); p != std::string::npos) {
    const std::string key = trim(cond.substr(0, p));
    if (!o.schema() || !o.schema()->field(key)) return true;
    const std::string have = o.get_string(key);
    const std::string list = cond.substr(p + 4);
    std::size_t start = 0;
    for (;;) {
      const auto bar = list.find('|', start);
      if (trim(list.substr(start, bar == std::string::npos ? bar : bar - start)) == have) return true;
      if (bar == std::string::npos) return false;
      start = bar + 1;
    }
  }
  const std::string key = trim(cond);
  if (!o.schema() || !o.schema()->field(key)) return true;
  return o.get_bool(key);
}

}  // namespace

OptionsEditor::OptionsEditor(QWidget* parent)
    : QWidget(parent),
      section_bar_(new QWidget(this)),
      section_buttons_(new QButtonGroup(this)),
      pages_(new QStackedWidget(this)) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(6);
  auto* flow = new FlowLayout(section_bar_);
  flow->setContentsMargins(6, 6, 6, 0);
  layout->addWidget(section_bar_);
  layout->addWidget(pages_, 1);
  section_buttons_->setExclusive(true);
  connect(section_buttons_, &QButtonGroup::idClicked, pages_, &QStackedWidget::setCurrentIndex);
}

QStringList OptionsEditor::sections() const {
  QStringList out;
  for (const QAbstractButton* b : section_buttons_->buttons()) out << b->text();
  return out;
}

QAbstractButton* OptionsEditor::section_button(int index) const { return section_buttons_->button(index); }

int OptionsEditor::current_section() const { return pages_->currentIndex(); }

void OptionsEditor::set_current_section(int index) {
  if (index < 0 || index >= pages_->count()) return;
  pages_->setCurrentIndex(index);
  if (QAbstractButton* b = section_buttons_->button(index)) b->setChecked(true);
}

void OptionsEditor::set_options(const pp::Options& options) {
  options_ = options;
  rebuild();
}

void OptionsEditor::set_quantity_choices(const QStringList& choices) {
  quantities_ = choices;
  rebuild();
}

QWidget* OptionsEditor::editor(const QString& key) const {
  auto it = editors_.find(key);
  return it == editors_.end() ? nullptr : it->second;
}

QWidget* OptionsEditor::row_editor(const QString& list, const QString& key) const {
  auto it = lists_.find(list);
  if (it == lists_.end()) return nullptr;
  auto e = it->second.editors.find(key);
  return e == it->second.editors.end() ? nullptr : e->second;
}

QListWidget* OptionsEditor::rows(const QString& list) const {
  auto it = lists_.find(list);
  return it == lists_.end() ? nullptr : it->second.list;
}

bool OptionsEditor::apply(const QString& key, const pp::OptionValue& value) {
  auto ok = options_.set(key.toStdString(), value);
  if (!ok) return false;
  rebuild();
  emit changed();
  return true;
}

QString OptionsEditor::row_label(const pp::Options& row, int index) const {
  QString label = QString::number(index + 1) + QStringLiteral(". ");
  if (!row.schema() || row.schema()->fields.empty()) return label;
  const auto& first = row.schema()->fields.front();
  QString v = qs(row.get_string(first.key));
  if (v.isEmpty()) v = tr("(auto)");
  if (row.schema()->field("enabled") && !row.get_bool("enabled")) v += tr(" (hidden)");
  return label + v;
}

QWidget* OptionsEditor::make_editor(const pp::FieldSpec& f, const pp::Options& values,
                                    std::function<Result<void>(pp::OptionValue)> set) {
  const pp::OptionValue current = values.get(f.key);
  auto report = [this](QWidget* w, const Result<void>& r) {
    mark(w, r ? QString() : qs(r.error().what));
    if (r) {
      refresh_enabled();
      emit changed();
    }
  };
  QWidget* w = nullptr;
  switch (f.type) {
    case pp::FieldType::Bool: {
      auto* box = new QCheckBox;
      box->setChecked(std::holds_alternative<bool>(current) && std::get<bool>(current));
      connect(box, &QCheckBox::toggled, this, [=, this](bool on) {
        if (!building_) report(box, set(on));
      });
      w = box;
      break;
    }
    case pp::FieldType::Int: {
      auto* spin = new QSpinBox;
      spin->setRange(static_cast<int>(std::max(f.min, -1e9)), static_cast<int>(std::min(f.max, 1e9)));
      spin->setValue(static_cast<int>(values.get_int(f.key)));
      connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [=, this](int v) {
        if (!building_) report(spin, set(static_cast<std::int64_t>(v)));
      });
      w = spin;
      break;
    }
    case pp::FieldType::Double: {
      auto* edit = new QLineEdit;
      if (const auto v = values.get_optional_double(f.key)) edit->setText(number_text(*v));
      if (f.optional) edit->setPlaceholderText(tr("auto"));
      connect(edit, &QLineEdit::editingFinished, this, [=, this] {
        if (building_) return;
        const QString t = edit->text().trimmed();
        if (t.isEmpty()) {
          report(edit, set(std::monostate{}));
          return;
        }
        bool ok = false;
        const double v = t.toDouble(&ok);
        if (!ok) {
          mark(edit, tr("not a number"));
          return;
        }
        report(edit, set(v));
      });
      w = edit;
      break;
    }
    case pp::FieldType::Enum: {
      auto* combo = new QComboBox;
      for (const auto& c : f.choices) combo->addItem(qs(c));
      combo->setCurrentText(qs(values.get_string(f.key)));
      connect(combo, &QComboBox::currentTextChanged, this, [=, this](const QString& t) {
        if (!building_) report(combo, set(t.toStdString()));
      });
      w = combo;
      break;
    }
    case pp::FieldType::Quantity: {
      auto* combo = new QComboBox;
      combo->setEditable(true);
      combo->addItems(quantities_);
      combo->setCurrentText(qs(values.get_string(f.key)));
      combo->setInsertPolicy(QComboBox::NoInsert);
      auto commit = [=, this] {
        if (!building_) report(combo, set(combo->currentText().trimmed().toStdString()));
      };
      connect(combo, qOverload<int>(&QComboBox::activated), this, commit);
      connect(combo->lineEdit(), &QLineEdit::editingFinished, this, commit);
      w = combo;
      break;
    }
    case pp::FieldType::Color: {
      auto* host = new QWidget;
      auto* h = new QHBoxLayout(host);
      h->setContentsMargins(0, 0, 0, 0);
      auto* button = new QPushButton;
      const QString c = qs(values.get_string(f.key));
      button->setText(c.isEmpty() ? tr("auto") : c);
      button->setStyleSheet(swatch_style(c));
      h->addWidget(button, 1);
      connect(button, &QPushButton::clicked, this, [=, this] {
        const QColor start(button->text() == tr("auto") ? theme().text.name() : button->text());
        const QColor chosen = QColorDialog::getColor(start, this, qs(f.label), QColorDialog::ShowAlphaChannel);
        if (!chosen.isValid()) return;
        const QString hex = chosen.alpha() == 255 ? chosen.name(QColor::HexRgb)
                                                  : chosen.name(QColor::HexRgb) +
                                                        QStringLiteral("%1").arg(chosen.alpha(), 2, 16, QLatin1Char('0'));
        auto r = set(hex.toStdString());
        if (r) {
          button->setText(hex);
          button->setStyleSheet(swatch_style(hex));
        }
        report(button, r);
      });
      if (f.optional) {
        auto* clear = new QToolButton;
        clear->setText(QStringLiteral("×"));
        clear->setToolTip(tr("Automatic"));
        h->addWidget(clear);
        connect(clear, &QToolButton::clicked, this, [=] {
          auto r = set(std::monostate{});
          if (r) {
            button->setText(tr("auto"));
            button->setStyleSheet(swatch_style(QString()));
          }
          report(button, r);
        });
      }
      w = host;
      break;
    }
    case pp::FieldType::StringList: {
      auto* edit = new QLineEdit;
      QStringList items;
      for (const auto& s : values.get_strings(f.key)) items << qs(s);
      edit->setText(items.join(QStringLiteral(", ")));
      edit->setPlaceholderText(tr("comma separated"));
      connect(edit, &QLineEdit::editingFinished, this, [=, this] {
        if (building_) return;
        std::vector<std::string> out;
        for (const auto& part : edit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
          const QString t = part.trimmed();
          if (!t.isEmpty()) out.push_back(t.toStdString());
        }
        report(edit, set(out));
      });
      w = edit;
      break;
    }
    case pp::FieldType::Font: {
      // The item data is the stored family; each installed family is shown in
      // its own face. A family that is not installed here (a preset from
      // another machine) keeps its entry, and is never handed to Qt as a font.
      auto* combo = new QComboBox;
      combo->addItem(tr("Default"), QString());
      for (const QString& family : font_families()) {
        combo->addItem(family, family);
        combo->setItemData(combo->count() - 1, QFont(family), Qt::FontRole);
      }
      const QString family = qs(values.get_string(f.key));
      int at = combo->findData(family);
      if (at < 0) at = combo->findData(family, Qt::UserRole, Qt::MatchFixedString);
      if (at < 0) {
        combo->insertItem(1, tr("%1 (not installed)").arg(family), family);
        at = 1;
      }
      combo->setCurrentIndex(at);
      connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=, this](int index) {
        if (!building_) report(combo, set(combo->itemData(index).toString().toStdString()));
      });
      w = combo;
      break;
    }
    case pp::FieldType::String: {
      auto* edit = new QLineEdit;
      edit->setText(qs(values.get_string(f.key)));
      connect(edit, &QLineEdit::editingFinished, this, [=, this] {
        if (!building_) report(edit, set(edit->text().toStdString()));
      });
      w = edit;
      break;
    }
  }
  w->setProperty("help", qs(f.help));
  w->setToolTip(qs(f.help));
  w->setProperty("enabled_when", qs(f.enabled_when));
  return w;
}

void OptionsEditor::rebuild() {
  building_ = true;
  const int section_shown = pages_->currentIndex();
  std::map<QString, int> selected_rows;
  for (const auto& [k, ui] : lists_) selected_rows[k] = ui.list ? ui.list->currentRow() : -1;
  while (pages_->count() > 0) {
    QWidget* w = pages_->widget(0);
    pages_->removeWidget(w);
    w->deleteLater();  // a rebuild may run inside a signal of one of these widgets
  }
  for (QAbstractButton* b : section_buttons_->buttons()) {
    section_buttons_->removeButton(b);
    section_bar_->layout()->removeWidget(b);
    b->deleteLater();
  }
  editors_.clear();
  lists_.clear();
  const auto& schema = options_.schema();
  if (!schema) {
    building_ = false;
    return;
  }

  std::vector<std::string> sections;
  auto section_of = [&](const std::string& s) {
    if (std::find(sections.begin(), sections.end(), s) == sections.end()) sections.push_back(s);
  };
  for (const auto& f : schema->fields) section_of(f.section);
  for (const auto& l : schema->lists) section_of(l.section);

  for (const auto& section : sections) {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    auto* form = new QFormLayout;
    v->addLayout(form);
    for (const auto& f : schema->fields) {
      if (f.section != section) continue;
      const std::string key = f.key;
      QWidget* e = make_editor(f, options_, [this, key](pp::OptionValue value) { return options_.set(key, std::move(value)); });
      form->addRow(qs(f.label), e);
      editors_[qs(f.key)] = e;
    }
    for (const auto& l : schema->lists) {
      if (l.section != section) continue;
      const QString lkey = qs(l.key);
      ListUi ui;
      auto* box = new QWidget;
      auto* lv = new QVBoxLayout(box);
      lv->setContentsMargins(0, 0, 0, 0);
      lv->addWidget(new QLabel(qs(l.label)));
      ui.list = new QListWidget;
      ui.list->setMaximumHeight(140);
      const auto initial = options_.rows(l.key);
      for (std::size_t i = 0; i < initial.size(); ++i) ui.list->addItem(row_label(initial[i], static_cast<int>(i)));
      lv->addWidget(ui.list);
      auto* buttons = new QHBoxLayout;
      auto* add = new QPushButton(tr("Add"));
      auto* remove = new QPushButton(tr("Remove"));
      auto* up = new QPushButton(tr("Up"));
      auto* down = new QPushButton(tr("Down"));
      for (auto* b : {add, remove, up, down}) buttons->addWidget(b);
      buttons->addStretch();
      lv->addLayout(buttons);
      ui.form_host = new QWidget;
      new QFormLayout(ui.form_host);
      lv->addWidget(ui.form_host);
      v->addWidget(box);
      lists_[lkey] = ui;

      const std::string key = l.key;
      auto edit_rows = [this, key, lkey](const std::function<int(std::vector<pp::Options>&, int)>& op) {
        auto rows = options_.rows(key);
        const int cur = lists_[lkey].list->currentRow();
        const int next = op(rows, cur);
        if (next == -2) return;
        if (options_.set_rows(key, rows)) {
          rebuild();
          if (lists_.count(lkey)) lists_[lkey].list->setCurrentRow(std::min<int>(next, static_cast<int>(rows.size()) - 1));
          emit changed();
        }
      };
      connect(add, &QPushButton::clicked, this, [this, key, edit_rows] {
        edit_rows([this, key](std::vector<pp::Options>& rows, int cur) {
          pp::Options row = cur >= 0 && cur < static_cast<int>(rows.size()) ? rows[cur] : options_.new_row(key);
          rows.push_back(row);
          return static_cast<int>(rows.size()) - 1;
        });
      });
      connect(remove, &QPushButton::clicked, this, [edit_rows] {
        edit_rows([](std::vector<pp::Options>& rows, int cur) {
          if (cur < 0 || cur >= static_cast<int>(rows.size())) return -2;
          rows.erase(rows.begin() + cur);
          return std::max(0, cur - 1);
        });
      });
      connect(up, &QPushButton::clicked, this, [edit_rows] {
        edit_rows([](std::vector<pp::Options>& rows, int cur) {
          if (cur <= 0 || cur >= static_cast<int>(rows.size())) return -2;
          std::swap(rows[cur], rows[cur - 1]);
          return cur - 1;
        });
      });
      connect(down, &QPushButton::clicked, this, [edit_rows] {
        edit_rows([](std::vector<pp::Options>& rows, int cur) {
          if (cur < 0 || cur + 1 >= static_cast<int>(rows.size())) return -2;
          std::swap(rows[cur], rows[cur + 1]);
          return cur + 1;
        });
      });
      connect(ui.list, &QListWidget::currentRowChanged, this, [this, lkey](int) {
        if (!building_) build_row_form(lkey);
      });
    }
    v->addStretch();
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* button = new QToolButton(section_bar_);
    button->setText(qs(section));
    button->setCheckable(true);
    button->setAutoRaise(true);
    section_buttons_->addButton(button, pages_->count());
    section_bar_->layout()->addWidget(button);
    pages_->addWidget(scroll);
  }
  for (auto& [k, ui] : lists_) {
    const int r = selected_rows.count(k) ? selected_rows[k] : -1;
    ui.list->setCurrentRow(r >= 0 && r < ui.list->count() ? r : (ui.list->count() > 0 ? 0 : -1));
  }
  set_current_section(section_shown >= 0 && section_shown < pages_->count() ? section_shown : 0);
  building_ = false;
  for (const auto& [k, _] : lists_) build_row_form(k);
  refresh_enabled();
}

void OptionsEditor::build_row_form(const QString& lkey) {
  auto it = lists_.find(lkey);
  if (it == lists_.end()) return;
  ListUi& ui = it->second;
  const bool was_building = building_;
  building_ = true;
  auto* form = static_cast<QFormLayout*>(ui.form_host->layout());
  while (form->rowCount() > 0) form->removeRow(0);
  ui.editors.clear();
  const int row = ui.list->currentRow();
  const std::string key = lkey.toStdString();
  const auto all = options_.rows(key);
  if (row >= 0 && row < static_cast<int>(all.size())) {
    const pp::Options& r = all[row];
    for (const auto& f : r.schema()->fields) {
      const std::string fkey = f.key;
      QWidget* e = make_editor(f, r, [this, key, row, fkey, lkey](pp::OptionValue value) -> Result<void> {
        auto rows = options_.rows(key);
        if (row >= static_cast<int>(rows.size())) return fail(ErrorKind::Config, "row is gone");
        auto ok = rows[row].set(fkey, std::move(value));
        if (!ok) return ok;
        if (auto s = options_.set_rows(key, rows); !s) return s;
        if (auto* item = lists_[lkey].list->item(row)) item->setText(row_label(rows[row], row));
        return {};
      });
      e->setProperty("row_list", lkey);
      form->addRow(qs(f.label), e);
      ui.editors[qs(f.key)] = e;
    }
  }
  building_ = was_building;
  refresh_enabled();
}

void OptionsEditor::refresh_enabled() {
  for (const auto& [key, w] : editors_) w->setEnabled(condition_holds(w->property("enabled_when").toString().toStdString(), options_));
  for (const auto& [lkey, ui] : lists_) {
    const int row = ui.list->currentRow();
    const auto rows = options_.rows(lkey.toStdString());
    if (row < 0 || row >= static_cast<int>(rows.size())) continue;
    for (const auto& [key, w] : ui.editors)
      w->setEnabled(condition_holds(w->property("enabled_when").toString().toStdString(), rows[row]));
  }
}

}  // namespace pychron::ui
