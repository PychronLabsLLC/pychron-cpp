#include "r_values_panel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <string>
#include <utility>

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

#include "error_report.hpp"
#include "pychron/core/user_file.hpp"

namespace pychron::ui {

namespace pp = pychron::processing;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString exact(double v) {
  std::array<char, 64> buf{};
  const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  return QString::fromLatin1(buf.data(), static_cast<qsizetype>(r.ptr - buf.data()));
}

QString header_text(const pp::GroupF& g, int nsigma) {
  if (!g.f) return qs(g.name);
  return qs(g.name) + QStringLiteral("\nF ") + qs(pp::r_value_text(g.f->value, g.f->error, nsigma));
}

}  // namespace

RValuesPanel::RValuesPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  auto* buttons = new QHBoxLayout;
  auto* copy_button = new QPushButton(tr("Copy"));
  copy_button->setToolTip(tr("The tables as shown, tab-separated"));
  connect(copy_button, &QPushButton::clicked, this, [this] { copy(); });
  auto* export_button = new QPushButton(tr("Export CSV..."));
  export_button->setToolTip(tr("Every ratio with its means, 1 sigma, at full precision"));
  connect(export_button, &QPushButton::clicked, this, [this] {
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Export R-values"), QStringLiteral("r_values.csv"), tr("CSV (*.csv)"));
    if (!path.isEmpty() && !export_csv(path))
      show_warning(this, "r_values", tr("Export"), tr("Could not write %1").arg(path));
  });
  buttons->addWidget(copy_button);
  buttons->addWidget(export_button);
  buttons->addStretch(1);
  layout->addLayout(buttons);

  auto* host = new QWidget;
  tables_layout_ = new QVBoxLayout(host);
  tables_layout_->setContentsMargins(0, 0, 0, 0);
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setWidget(host);
  layout->addWidget(scroll, 1);

  note_ = new QLabel(QString::fromUtf8(pp::kRValueNote.data(), static_cast<qsizetype>(pp::kRValueNote.size())));
  note_->setWordWrap(true);
  layout->addWidget(note_);
}

void RValuesPanel::set_matrices(std::vector<pp::RValueMatrix> matrices, const pp::Options& ideogram_options) {
  matrices_ = std::move(matrices);
  options_ = ideogram_options;
  nsigma_ = std::max<int>(1, static_cast<int>(options_.get_int("error_bar_nsigma")));
  for (auto* w : table_widgets_) delete w;  // Qt: the layout forgets a deleted child
  table_widgets_.clear();
  tables_.clear();
  for (const auto& m : matrices_) {
    if (m.groups.size() < 2) continue;
    auto* box = new QWidget;
    auto* bl = new QVBoxLayout(box);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->addWidget(new QLabel(qs(m.graph_name)));
    const int n = static_cast<int>(m.groups.size());
    auto* t = new QTableWidget(n, n);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QStringList headers;
    for (const auto& g : m.groups) headers << header_text(g, nsigma_);
    t->setHorizontalHeaderLabels(headers);
    t->setVerticalHeaderLabels(headers);
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j) {
        if (i == j) continue;
        auto* item = new QTableWidgetItem;
        if (const auto& c = m.cells[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]) {
          item->setText(qs(pp::r_value_text(c->r, c->r_err, nsigma_)));
          item->setToolTip(tr("R = %1, 1σ = %2").arg(exact(c->r), exact(c->r_err)));
        } else {
          const auto& gi = m.groups[static_cast<std::size_t>(i)];
          const auto& gj = m.groups[static_cast<std::size_t>(j)];
          item->setToolTip(qs(!gi.f ? gi.error : !gj.f ? gj.error : std::string("F of the denominator is 0")));
        }
        t->setItem(i, j, item);
      }
    t->resizeColumnsToContents();
    t->resizeRowsToContents();
    bl->addWidget(t);
    tables_layout_->addWidget(box);
    table_widgets_ << box;
    tables_ << t;
  }
}

bool RValuesPanel::has_tables() const { return !tables_.isEmpty(); }

QString RValuesPanel::note_text() const { return note_->text(); }

bool RValuesPanel::export_csv(const QString& path) const {
  const std::string text = pp::r_values_csv(matrices_, options_);
  // A sibling temporary file renamed over the destination: a file there is whole or as it was.
  QSaveFile file(path);
  const auto size = static_cast<qint64>(text.size());
  if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), size) != size || !file.commit()) return false;
  pychron::mark_as_user_file(std::filesystem::path(path.toStdU16String()));
  return true;
}

void RValuesPanel::copy() const { QApplication::clipboard()->setText(qs(pp::r_values_tsv(matrices_, nsigma_))); }

}  // namespace pychron::ui
