// The data browser's Selected list: unique analyses, newest first.

#include <QtTest/QtTest>

#include <string>
#include <vector>

#include "selection_list.hpp"

namespace pp = pychron::processing;
using pychron::ui::SelectionList;

namespace {

pp::AnalysisSummary row(const char* uuid, double t, const char* tag = "ok") {
  pp::AnalysisSummary s;
  s.uuid = uuid;
  s.timestamp = t;
  s.tag = tag;
  return s;
}

std::vector<std::string> order(const SelectionList& list) {
  std::vector<std::string> out;
  for (const auto& r : list.rows()) out.push_back(r.uuid);
  return out;
}

using Uuids = std::vector<std::string>;

}  // namespace

class SelectionListTest : public QObject {
  Q_OBJECT

 private slots:
  void add_sorts_newest_first_and_ignores_duplicates() {
    SelectionList list;
    QCOMPARE(list.add({row("a", 1), row("c", 3)}), std::size_t{2});
    QCOMPARE(list.add({row("b", 2), row("c", 3)}), std::size_t{1});
    QCOMPARE(order(list), (Uuids{"c", "b", "a"}));
    QVERIFY(list.contains("b"));
    QVERIFY(!list.contains("z"));
    QCOMPARE(list.size(), std::size_t{3});
  }

  void equal_timestamps_order_by_uuid_descending() {
    SelectionList list;
    list.add({row("x", 5), row("y", 5)});
    QCOMPARE(order(list), (Uuids{"y", "x"}));
  }

  void remove_and_clear() {
    SelectionList list;
    list.add({row("a", 1), row("b", 2)});
    QCOMPARE(list.remove({"b", "nope"}), std::size_t{1});
    QCOMPARE(order(list), (Uuids{"a"}));
    QVERIFY(!list.contains("b"));
    list.clear();
    QVERIFY(list.empty());
  }

  void replace_from_updates_and_drops() {
    SelectionList list;
    list.add({row("a", 1), row("b", 2, "ok"), row("c", 3)});
    QCOMPARE(list.replace_from({row("b", 2, "invalid"), row("c", 3), row("zzz", 9)}), std::size_t{1});
    QCOMPARE(order(list), (Uuids{"c", "b"}));
    QCOMPARE(list.rows()[1].tag, std::string("invalid"));
    QVERIFY(!list.contains("zzz"));
  }

  void uuids_are_sorted() {
    SelectionList list;
    list.add({row("c", 3), row("a", 1), row("b", 2)});
    QCOMPARE(list.uuids(), (Uuids{"a", "b", "c"}));
  }
};

QTEST_MAIN(SelectionListTest)
#include "test_selection_list.moc"
