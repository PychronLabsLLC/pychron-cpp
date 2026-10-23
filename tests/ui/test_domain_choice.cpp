// Which domain pychron-ui runs on: the install's id, against the domains this build links.

#include <string>

#include <QString>
#include <QtTest/QtTest>

#include "domain_choice.hpp"
#include "pychron/processing/domain.hpp"

using pychron::ui::choose_domain;
namespace pp = pychron::processing;

class TestDomainChoice : public QObject {
  Q_OBJECT

 private slots:
  void arar_and_noble_gas_resolve_to_their_own_domains() {
    pp::DomainRegistry registry;
    std::string error;
    const auto* arar = choose_domain(registry, "arar", error);
    QVERIFY2(arar != nullptr, error.c_str());
    QCOMPARE(QString::fromStdString(std::string(arar->id())), QStringLiteral("arar"));
    QVERIFY(error.empty());

    // A registry takes the domains again on each call, replacing the ones it holds.
    pp::DomainRegistry other;
    const auto* noble = choose_domain(other, "noble_gas", error);
    QVERIFY2(noble != nullptr, error.c_str());
    QCOMPARE(QString::fromStdString(std::string(noble->id())), QStringLiteral("noble_gas"));
  }

  void an_unknown_id_is_refused_and_names_the_known_ones() {
    pp::DomainRegistry registry;
    std::string error;
    QVERIFY(choose_domain(registry, "u_pb", error) == nullptr);
    QCOMPARE(QString::fromStdString(error), QStringLiteral("unknown domain 'u_pb'; known: arar, noble_gas"));
  }
};

QTEST_MAIN(TestDomainChoice)
#include "test_domain_choice.moc"
