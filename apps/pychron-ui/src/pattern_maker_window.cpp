#include "pattern_maker_window.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QVBoxLayout>

#include "theme.hpp"

namespace pychron::ui {

namespace {

constexpr laser::PatternKind kKinds[] = {
    laser::PatternKind::Polygon,      laser::PatternKind::Linear,   laser::PatternKind::CircularContour,
    laser::PatternKind::LineSpiral,   laser::PatternKind::SquareSpiral, laser::PatternKind::Random,
    laser::PatternKind::Rubberband,   laser::PatternKind::Raster,   laser::PatternKind::Trough,
    laser::PatternKind::Dragonfly};

QString kind_name(laser::PatternKind kind) { return QString::fromStdString(std::string(to_string(kind))); }

QString field_name(std::string_view key) { return QStringLiteral("field_") + QString::fromUtf8(key.data(), qsizetype(key.size())); }

QString label_of(std::string_view key) {
  QString text = QString::fromUtf8(key.data(), qsizetype(key.size()));
  text.replace(QLatin1Char('_'), QLatin1Char(' '));
  text[0] = text[0].toUpper();
  return text;
}

// What a key is measured in.
QString unit_of(std::string_view key) {
  if (key == "velocity") return QStringLiteral(" mm/s");
  if (key == "rotation") return QStringLiteral(" °");
  if (key == "duration") return QStringLiteral(" s");
  for (std::string_view length : {"radius", "length", "width", "offset", "dx", "walk_x", "walk_y", "perimeter_radius",
                                  "move_threshold", "max_step", "spiral_base", "target_radius"}) {
    if (key == length) return QStringLiteral(" mm");
  }
  return {};
}

QString num(double value, int places) { return QString::number(value, 'f', places); }

// A number shown as it is, not rounded to a few places: a pattern opened and
// saved is then the pattern it was. No trailing zeros.
class ExactSpin : public QDoubleSpinBox {
 public:
  explicit ExactSpin(QWidget* parent) : QDoubleSpinBox(parent) { setDecimals(15); }
  QString textFromValue(double value) const override { return QLocale::c().toString(value, 'g', 15); }
};

}  // namespace

// The path, from its center: drawn to fit, +y up.
class PatternPreview : public QWidget {
 public:
  explicit PatternPreview(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("preview"));
    setMinimumSize(260, 260);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  }
  void set_path(std::vector<laser::StageXY> path) {
    path_ = std::move(path);
    perimeter_ = 0;
    message_.clear();
    update();
  }
  // A pattern with no path: the circle it stays within.
  void set_perimeter(double radius_mm, const QString& message) {
    path_.clear();
    perimeter_ = radius_mm;
    message_ = message;
    update();
  }
  void set_message(const QString& message) {
    path_.clear();
    perimeter_ = 0;
    message_ = message;
    update();
  }
  int points() const { return static_cast<int>(path_.size()); }

 protected:
  void paintEvent(QPaintEvent*) override {
    const Theme& t = theme();
    QPainter p(this);
    p.fillRect(rect(), t.plot_bg);
    p.setRenderHint(QPainter::Antialiasing, true);
    // the extent, always about the center
    double reach = perimeter_;
    for (const auto& point : path_) reach = std::max({reach, std::abs(point.x), std::abs(point.y)});
    if (!(reach > 0)) reach = 1;
    const double scale = (std::min(width(), height()) / 2.0 - 16) / reach;
    const QPointF center(width() / 2.0, height() / 2.0);
    const auto at = [&](const laser::StageXY& point) { return QPointF(center.x() + scale * point.x, center.y() - scale * point.y); };
    p.setPen(QPen(t.grid, 1));
    p.drawLine(QPointF(0, center.y()), QPointF(width(), center.y()));
    p.drawLine(QPointF(center.x(), 0), QPointF(center.x(), height()));
    if (perimeter_ > 0) {
      p.setPen(QPen(t.accent, 1, Qt::DashLine));
      p.setBrush(Qt::NoBrush);
      p.drawEllipse(center, perimeter_ * scale, perimeter_ * scale);
    }
    if (!path_.empty()) {
      QPolygonF line;
      line << center;
      for (const auto& point : path_) line << at(point);
      p.setPen(QPen(t.accent_strong, 1.5));
      p.setBrush(Qt::NoBrush);
      p.drawPolyline(line);
      // where the beam goes first
      p.setPen(Qt::NoPen);
      p.setBrush(t.ok);
      p.drawEllipse(at(path_.front()), 3.5, 3.5);
    }
    // the center: where the stage is when the pattern starts, and ends
    p.setPen(QPen(t.error, 1));
    p.drawLine(center + QPointF(-5, 0), center + QPointF(5, 0));
    p.drawLine(center + QPointF(0, -5), center + QPointF(0, 5));
    p.setPen(t.muted_text);
    p.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignBottom | Qt::AlignRight, tr("± %1 mm").arg(num(reach, 2)));
    if (!message_.isEmpty()) p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap, message_);
  }

 private:
  std::vector<laser::StageXY> path_;
  double perimeter_ = 0;
  QString message_;
};

PatternMakerWindow::PatternMakerWindow(laser::PatternLibrary& library, std::filesystem::path dir, QWidget* parent)
    : QMainWindow(parent), library_(library), dir_(std::move(dir)) {
  setObjectName(QStringLiteral("pattern_maker"));
  setWindowTitle(tr("Pattern maker"));
  auto* central = new QWidget(this);
  auto* row = new QHBoxLayout(central);

  auto* left = new QVBoxLayout;
  auto* top = new QHBoxLayout;
  auto* fresh = new QPushButton(tr("New"), central);
  fresh->setObjectName(QStringLiteral("new"));
  top->addWidget(fresh);
  open_list_ = new QComboBox(central);
  open_list_->setObjectName(QStringLiteral("open_list"));
  top->addWidget(open_list_, 1);
  left->addLayout(top);

  auto* head = new QFormLayout;
  name_ = new QLineEdit(central);
  name_->setObjectName(QStringLiteral("name"));
  name_->setPlaceholderText(tr("what a queue calls it"));
  head->addRow(tr("Name"), name_);
  kind_choice_ = new QComboBox(central);
  kind_choice_->setObjectName(QStringLiteral("kind"));
  for (const laser::PatternKind kind : kKinds) kind_choice_->addItem(kind_name(kind));
  head->addRow(tr("Kind"), kind_choice_);
  left->addLayout(head);

  fields_ = new QWidget(central);
  form_ = new QFormLayout(fields_);
  form_->setContentsMargins(0, 0, 0, 0);
  left->addWidget(fields_);
  left->addStretch(1);

  problem_ = new QLabel(central);
  problem_->setObjectName(QStringLiteral("problem"));
  problem_->setWordWrap(true);
  style::set_tone(problem_, style::Tone::Error);
  left->addWidget(problem_);
  save_ = new QPushButton(tr("Save"), central);
  save_->setObjectName(QStringLiteral("save"));
  left->addWidget(save_);
  row->addLayout(left, 0);

  auto* right = new QVBoxLayout;
  preview_ = new PatternPreview(central);
  right->addWidget(preview_, 1);
  summary_ = new QLabel(central);
  summary_->setObjectName(QStringLiteral("summary"));
  summary_->setWordWrap(true);
  right->addWidget(summary_);
  row->addLayout(right, 1);
  setCentralWidget(central);
  resize(760, 520);

  connect(fresh, &QPushButton::clicked, this, [this] { show_pattern(laser::Pattern::defaults(laser::PatternKind::Polygon)); });
  connect(open_list_, &QComboBox::activated, this, [this](int index) {
    if (index > 0) open(open_list_->itemText(index));
  });
  connect(name_, &QLineEdit::textChanged, this, [this] { changed(); });
  connect(kind_choice_, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (filling_ || index < 0) return;
    // another kind starts from legacy's defaults for it
    laser::Pattern next = laser::Pattern::defaults(kKinds[index]);
    kind_ = next.kind;
    build_fields(next);
    changed();
  });
  connect(save_, &QPushButton::clicked, this, [this] { save(); });

  refresh_open_list();
  show_pattern(laser::Pattern::defaults(laser::PatternKind::Polygon));
}

void PatternMakerWindow::refresh_open_list() {
  open_list_->clear();
  open_list_->addItem(tr("Open…"));
  for (const auto& name : library_.names()) open_list_->addItem(QString::fromStdString(name));
}

void PatternMakerWindow::open(const QString& name) {
  if (const auto pattern = library_.find(name.toStdString())) show_pattern(*pattern);
}

void PatternMakerWindow::show_pattern(const laser::Pattern& pattern) {
  filling_ = true;
  kind_ = pattern.kind;
  name_->setText(QString::fromStdString(pattern.name));
  kind_choice_->setCurrentIndex(kind_choice_->findText(kind_name(pattern.kind)));
  filling_ = false;
  build_fields(pattern);
  note_.clear();
  refresh();
}

void PatternMakerWindow::build_fields(const laser::Pattern& pattern) {
  filling_ = true;
  // Deleted now, not later: a field of the last kind must not be found.
  while (form_->rowCount() > 0) form_->removeRow(0);
  for (const laser::PatternField& field : laser::pattern_fields(pattern.kind)) {
    const double value = laser::field_value(pattern, field.key).value_or(0);
    QWidget* editor = nullptr;
    if (field.type == laser::PatternField::Type::Flag) {
      auto* box = new QCheckBox(fields_);
      box->setChecked(value != 0);
      connect(box, &QCheckBox::toggled, this, [this] { changed(); });
      editor = box;
    } else if (field.type == laser::PatternField::Type::Whole) {
      auto* spin = new QSpinBox(fields_);
      spin->setRange(static_cast<int>(field.low), static_cast<int>(field.high));
      spin->setValue(static_cast<int>(value));
      connect(spin, &QSpinBox::valueChanged, this, [this] { changed(); });
      editor = spin;
    } else {
      auto* spin = new ExactSpin(fields_);
      // The range's own end may be typed: what is wrong with it is then said.
      spin->setRange(field.low, field.high);
      spin->setSingleStep(field.key == "rotation" ? 5 : 0.1);
      spin->setSuffix(unit_of(field.key));
      spin->setValue(value);
      connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
      editor = spin;
    }
    editor->setObjectName(field_name(field.key));
    if (field.key == "duration") editor->setToolTip(tr("0: for as long as the run heats"));
    form_->addRow(label_of(field.key), editor);
  }
  if (pattern.kind == laser::PatternKind::Random) {
    auto* fixed = new QCheckBox(tr("the same walk each run"), fields_);
    fixed->setObjectName(QStringLiteral("field_seed_fixed"));
    fixed->setChecked(pattern.seed.has_value());
    // Text, not a spin box: a seed is any whole number a file can hold.
    auto* seed = new QLineEdit(fields_);
    seed->setObjectName(QStringLiteral("field_seed"));
    seed->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{1,19}")), seed));
    seed->setText(QString::number(pattern.seed.value_or(0)));
    seed->setEnabled(pattern.seed.has_value());
    connect(fixed, &QCheckBox::toggled, this, [this, seed](bool on) {
      seed->setEnabled(on);
      changed();
    });
    connect(seed, &QLineEdit::textChanged, this, [this] { changed(); });
    form_->addRow(tr("Seed"), seed);
    form_->addRow(QString(), fixed);
  }
  if (pattern.kind == laser::PatternKind::Dragonfly) {
    auto* spiral = new QComboBox(fields_);
    spiral->setObjectName(QStringLiteral("field_spiral"));
    spiral->addItems({QStringLiteral("hexagon"), QStringLiteral("square")});
    spiral->setCurrentIndex(pattern.square_spiral ? 1 : 0);
    spiral->setToolTip(tr("How it searches when the glow is lost"));
    connect(spiral, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    form_->addRow(tr("Spiral"), spiral);
  }
  filling_ = false;
}

laser::Pattern PatternMakerWindow::pattern() const {
  laser::Pattern p = laser::Pattern::defaults(kind_);
  p.name = name_->text().toStdString();
  for (const laser::PatternField& field : laser::pattern_fields(kind_)) {
    const QString name = field_name(field.key);
    if (const auto* box = fields_->findChild<QCheckBox*>(name)) laser::set_field(p, field.key, box->isChecked() ? 1 : 0);
    else if (const auto* whole = fields_->findChild<QSpinBox*>(name)) laser::set_field(p, field.key, whole->value());
    else if (const auto* number = fields_->findChild<QDoubleSpinBox*>(name)) laser::set_field(p, field.key, number->value());
  }
  if (kind_ == laser::PatternKind::Random) {
    const auto* fixed = fields_->findChild<QCheckBox*>(QStringLiteral("field_seed_fixed"));
    const auto* seed = fields_->findChild<QLineEdit*>(QStringLiteral("field_seed"));
    if (fixed != nullptr && seed != nullptr && fixed->isChecked()) p.seed = seed->text().toULongLong();
  }
  if (kind_ == laser::PatternKind::Dragonfly) {
    if (const auto* spiral = fields_->findChild<QComboBox*>(QStringLiteral("field_spiral"))) {
      p.square_spiral = spiral->currentIndex() == 1;
    }
  }
  return p;
}

int PatternMakerWindow::preview_points() const { return preview_->points(); }

void PatternMakerWindow::changed() {
  if (filling_) return;
  note_.clear();
  refresh();
}

void PatternMakerWindow::refresh() {
  const laser::Pattern p = pattern();
  // The pattern itself first: a form with no name yet still shows its path.
  laser::Pattern unnamed = p;
  unnamed.name = "pattern";
  const auto sound = laser::check_pattern(unnamed);
  QString problem;
  if (!sound) {
    problem = QString::fromStdString(sound.error().what);
    if (problem.startsWith(QStringLiteral("pattern: "))) problem.remove(0, 9);
    preview_->set_message(tr("no path: see below"));
    summary_->clear();
  } else if (p.follows_glow()) {
    preview_->set_perimeter(p.perimeter_radius, tr("follows the glow: no path"));
    summary_->setText(tr("follows the glow for %1, never further than %2 mm from where it starts")
                          .arg(p.duration_s > 0 ? tr("the run's duration (else %1 s)").arg(num(p.duration_s, 1))
                                                : tr("the run's duration"),
                               num(p.perimeter_radius, 2)));
  } else if (auto path = laser::pattern_path(p, p.seed.value_or(0))) {
    const double length = laser::path_length(*path);
    QString text = tr("%1 points · %2 mm · %3 s at %4 mm/s")
                       .arg(path->size())
                       .arg(num(length, 2), num(length / p.velocity, 1), num(p.velocity, 2));
    if (p.kind == laser::PatternKind::Random && !p.seed) text += tr(" · a new walk each run (one is shown)");
    summary_->setText(text);
    preview_->set_path(std::move(*path));
  } else {
    problem = QString::fromStdString(path.error().what);
    preview_->set_message(tr("no path: see below"));
    summary_->clear();
  }
  if (problem.isEmpty()) {
    if (const auto named = laser::check_pattern(p); !named) problem = QString::fromStdString(named.error().what);
  }
  if (!note_.isEmpty() && problem.isEmpty()) summary_->setText(note_ + QStringLiteral(" · ") + summary_->text());
  problem_->setText(problem);
  save_->setEnabled(problem.isEmpty());
}

void PatternMakerWindow::save() {
  const laser::Pattern p = pattern();
  const bool replaced = library_.find(p.name) != nullptr;
  const auto file = laser::save_pattern(dir_, p);
  if (!file) {
    problem_->setText(QString::fromStdString(file.error().what));
    return;
  }
  library_.put(p);
  refresh_open_list();
  note_ = (replaced ? tr("saved over %1") : tr("saved %1")).arg(QString::fromStdString(file->filename().string()));
  refresh();
  emit saved(QString::fromStdString(p.name));
}

}  // namespace pychron::ui
