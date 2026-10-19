// The editors of a setup question (question_editor.hpp), shared by the setup
// wizard and Preferences > Data source: a value set reads back the same, a
// path has its Browse button, a port out of range is refused.

#include <QtTest>

#include <QAbstractButton>
#include <QButtonGroup>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QWidget>

#include <cstdint>
#include <memory>
#include <string>

#include "pychron/setup/profile.hpp"
#include "pychron/setup/template.hpp"
#include "question_editor.hpp"

using namespace pychron;
using namespace pychron::ui;

namespace {

setup::Question question(const char* id, setup::QuestionType type) {
  setup::Question q;
  q.id = id;
  q.prompt = id;
  q.type = type;
  return q;
}

std::string text_of(const Result<setup::Value>& v) { return v ? setup::to_text(*v) : "error: " + v.error().what; }

}  // namespace

class TestQuestionEditor : public QObject {
  Q_OBJECT

 private slots:
  void aPathHasALineEditAndBrowse() {
    QWidget parent;
    const auto q = question("db_file", setup::QuestionType::Path);
    QWidget* editor = make_question_editor(q, &parent);
    QVERIFY(editor != nullptr);
    QCOMPARE(editor->parentWidget(), &parent);
    QVERIFY(editor->findChild<QLineEdit*>(QStringLiteral("text")) != nullptr);
    bool browse = false;
    for (auto* b : editor->findChildren<QPushButton*>()) browse = browse || b->text() == QStringLiteral("Browse…");
    QVERIFY(browse);
    set_question_value(editor, q, setup::Value{std::string("data/x.db")});
    QCOMPARE(text_of(question_value(editor, q)), std::string("data/x.db"));
  }

  void aChoiceRoundTrips() {
    QWidget parent;
    auto q = question("data_source", setup::QuestionType::Choice);
    q.choices = {"local", "server"};
    QWidget* editor = make_question_editor(q, &parent);
    QVERIFY(editor->findChild<QButtonGroup*>() != nullptr);
    QCOMPARE(editor->findChildren<QRadioButton*>().size(), 2);
    QVERIFY(!question_value(editor, q));  // nothing chosen yet
    set_question_value(editor, q, setup::Value{std::string("server")});
    QCOMPARE(text_of(question_value(editor, q)), std::string("server"));
    set_question_value(editor, q, setup::Value{std::string("local")});
    QCOMPARE(text_of(question_value(editor, q)), std::string("local"));
  }

  // The wizard's refresh runs once a click, not for the radio switched off
  // too; a value set from code is heard as well (review finding 8).
  void aChoiceIsHeardOnceAClick() {
    QWidget parent;
    auto q = question("data_source", setup::QuestionType::Choice);
    q.choices = {"local", "server"};
    int heard = 0;
    QWidget* editor = make_question_editor(q, &parent, [&heard] { ++heard; });
    set_question_value(editor, q, setup::Value{std::string("local")});
    QCOMPARE(heard, 1);
    editor->findChild<QRadioButton*>(QStringLiteral("data_source-server"))->click();
    QCOMPARE(heard, 2);
    editor->findChild<QRadioButton*>(QStringLiteral("data_source-server"))->click();  // already on
    QCOMPARE(heard, 2);
    QCOMPARE(text_of(question_value(editor, q)), std::string("server"));
  }

  void aPortOutOfRangeIsRefused() {
    QWidget parent;
    const auto q = question("db_port", setup::QuestionType::Port);
    QWidget* editor = make_question_editor(q, &parent);
    set_question_value(editor, q, setup::Value{std::int64_t{5433}});
    QCOMPARE(text_of(question_value(editor, q)), std::string("5433"));
    set_question_value(editor, q, setup::Value{std::int64_t{70000}});
    const auto refused = question_value(editor, q);
    QVERIFY(!refused);
    QVERIFY2(refused.error().what.find("port") != std::string::npos, refused.error().what.c_str());
    set_question_value(editor, q, setup::Value{std::int64_t{5432}});
    QCOMPARE(text_of(question_value(editor, q)), std::string("5432"));
  }

  void aSecretIsNotEchoed() {
    QWidget parent;
    const auto q = question("db_password", setup::QuestionType::Secret);
    QWidget* editor = make_question_editor(q, &parent);
    auto* edit = qobject_cast<QLineEdit*>(editor);
    QVERIFY(edit != nullptr);
    QCOMPARE(edit->echoMode(), QLineEdit::Password);
    set_question_value(editor, q, setup::Value{std::string(" p@ss ")});
    QCOMPARE(text_of(question_value(editor, q)), std::string(" p@ss "));  // as typed, not trimmed
  }

  void aChangeIsHeard() {
    QWidget parent;
    const auto q = question("db_host", setup::QuestionType::Host);
    int heard = 0;
    QWidget* editor = make_question_editor(q, &parent, [&heard] { ++heard; });
    set_question_value(editor, q, setup::Value{std::string("db.lab")});
    QVERIFY(heard > 0);
    QCOMPARE(text_of(question_value(editor, q)), std::string("db.lab"));
  }
};

QTEST_MAIN(TestQuestionEditor)
#include "test_question_editor.moc"
