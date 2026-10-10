#pragma once

// An IStore that forwards to another and fails on request, for the suites
// that test a persister against a store that goes away or loses an
// acknowledgement. Derives from the ingest suite's forwarding store, so a new
// IStore method needs a forwarder there only.

#include <string>

#include "../ingest/forwarding_store.hpp"

namespace pychron::experiment::testing {

class FailingStore final : public ingest::testing::ForwardingStore {
 public:
  using ForwardingStore::ForwardingStore;

  // The next `times` calls of ingest fail without reaching the store.
  void fail_ingest(int times) { fail_ingest_ = times; }
  // The next `times` calls of ingest reach the store, commit, and then report
  // an Io error: the acknowledgement was lost.
  void fail_after_commit(int times) { fail_after_commit_ = times; }
  // While set, the catalog and analysis lookups a persister makes fail.
  void fail_reads(bool on) { fail_reads_ = on; }

  Result<persistence::IngestAck> ingest(const persistence::IngestItem& item) override {
    if (fail_ingest_ > 0) {
      --fail_ingest_;
      return fail(ErrorKind::Io, "ingest failed (test)");
    }
    auto ack = ForwardingStore::ingest(item);
    if (ack && fail_after_commit_ > 0) {
      --fail_after_commit_;
      return fail(ErrorKind::Io, "acknowledgement lost (test)");
    }
    return ack;
  }

  Result<std::optional<persistence::Uuid>> find_identifier(const std::string& identifier) override {
    if (fail_reads_) return read_error();
    return ForwardingStore::find_identifier(identifier);
  }
  Result<std::optional<persistence::Uuid>> find_analysis(const std::string& identifier, int aliquot,
                                                         int increment) override {
    if (fail_reads_) return read_error();
    return ForwardingStore::find_analysis(identifier, aliquot, increment);
  }
  Result<std::optional<persistence::LastAnalysis>> last_analysis(const std::string& identifier) override {
    if (fail_reads_) return read_error();
    return ForwardingStore::last_analysis(identifier);
  }
  Result<std::optional<persistence::Uuid>> find_catalog_row(
      persistence::CatalogTable table, const std::vector<persistence::CatalogKeyPart>& key) override {
    if (fail_reads_) return read_error();
    return ForwardingStore::find_catalog_row(table, key);
  }

 private:
  static Unexpected<Error> read_error() { return fail(ErrorKind::Io, "connection lost (test)"); }

  int fail_ingest_ = 0;
  int fail_after_commit_ = 0;
  bool fail_reads_ = false;
};

}  // namespace pychron::experiment::testing
