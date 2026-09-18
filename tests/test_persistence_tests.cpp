// Traffic Engineering Fabric - durable storage and recovery proofs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "support/fixtures.hpp"
#include "support/test_framework.hpp"
#include "tef/engine.hpp"
#include "tef/persist.hpp"
#include "tef/solver.hpp"

using namespace tef;
using tef::test::build_snapshot;
using tef::test::simple_fabric;
using tef::test::SnapshotSpec;

namespace {

PlanRepository::Options options_for(const std::string& directory, std::size_t compact_after = 4096) {
  PlanRepository::Options options;
  options.directory = directory;
  options.fsync = true;
  options.compact_after_records = compact_after;
  return options;
}

AuditRecord audit(std::uint64_t tick, std::string event) {
  AuditRecord record;
  record.tick = tick;
  record.event = std::move(event);
  record.detail = "durable audit entry";
  record.subject = Sha256::hash(std::string("subject") + std::to_string(tick));
  return record;
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
  std::vector<std::byte> bytes;
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) return bytes;
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    bytes.resize(read);
  }
  std::fclose(file);
  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  TEF_CHECK(file != nullptr);
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    TEF_CHECK_EQ(written, bytes.size());
  }
  std::fclose(file);
}

std::uintmax_t journal_size(const std::filesystem::path& path) {
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(path, code);
  return code ? 0 : size;
}

}  // namespace

TEF_TEST(repository_survives_a_real_close_and_reopen) {
  const std::string directory = tef::test::temporary_directory("reopen");
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    for (std::uint64_t i = 1; i <= 20; ++i) {
      const Status appended =
          repository.value()->append(RecordType::audit, encode_audit(audit(i, "entry")));
      TEF_CHECK_MSG(appended.ok(), appended.format());
    }
    TEF_CHECK_EQ(repository.value()->record_count(), std::size_t{20});
    TEF_CHECK_EQ(repository.value()->last_sequence(), std::uint64_t{20});
    TEF_CHECK(repository.value()->close().ok());
  }
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    TEF_CHECK_EQ(repository.value()->record_count(), std::size_t{20});
    TEF_CHECK_EQ(repository.value()->last_sequence(), std::uint64_t{20});
    TEF_CHECK(repository.value()->recovery().manifest_present);
    TEF_CHECK_EQ(repository.value()->recovery().journal_bytes_truncated, std::uint64_t{0});
    const std::vector<DurableRecord> records = repository.value()->records();
    TEF_CHECK_EQ(records.size(), std::size_t{20});
    for (std::size_t i = 0; i < records.size(); ++i) {
      TEF_CHECK_EQ(records[i].sequence, static_cast<std::uint64_t>(i) + 1);
      TEF_CHECK_EQ(records[i].type, RecordType::audit);
      if (i > 0) TEF_CHECK(records[i].previous_digest == records[i - 1].record_digest);
    }
    TEF_CHECK(repository.value()->close().ok());
  }
}

TEF_TEST(appends_are_durable_at_the_moment_they_return) {
  const std::string directory = tef::test::temporary_directory("durable-append");
  Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
  TEF_CHECK(repository.has_value());
  TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(1, "a"))).ok());
  // The journal file must already contain the record before the append returns;
  // the process is not closed and nothing is buffered by the caller.
  const std::filesystem::path journal = std::filesystem::path(directory) / "journal.tefj";
  const std::uintmax_t after_first = journal_size(journal);
  TEF_CHECK(after_first > 32);
  TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(2, "b"))).ok());
  TEF_CHECK(journal_size(journal) > after_first);
  TEF_CHECK(repository.value()->close().ok());
}

TEF_TEST(a_torn_journal_tail_is_truncated_not_reinterpreted) {
  const std::string directory = tef::test::temporary_directory("torn-journal");
  std::uintmax_t clean_size = 0;
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    for (std::uint64_t i = 1; i <= 10; ++i) {
      TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(i, "entry"))).ok());
    }
    TEF_CHECK(repository.value()->close().ok());
    clean_size = journal_size(std::filesystem::path(directory) / "journal.tefj");
  }

  // Simulate a crash in the middle of a record write: the file grows by a
  // partial record whose CRC and digest cannot match.
  const std::filesystem::path journal = std::filesystem::path(directory) / "journal.tefj";
  std::vector<std::byte> bytes = read_file(journal);
  TEF_CHECK_EQ(bytes.size(), clean_size);
  for (int i = 0; i < 23; ++i) bytes.push_back(static_cast<std::byte>(0xAB));
  write_file(journal, bytes);
  TEF_CHECK(journal_size(journal) > clean_size);

  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    TEF_CHECK_EQ(repository.value()->record_count(), std::size_t{10});
    TEF_CHECK_EQ(repository.value()->last_sequence(), std::uint64_t{10});
    TEF_CHECK(repository.value()->recovery().journal_bytes_truncated > 0);
    TEF_CHECK_EQ(journal_size(journal), clean_size);
    // The repository is still writable after the repairs.
    TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(11, "after"))).ok());
    TEF_CHECK_EQ(repository.value()->last_sequence(), std::uint64_t{11});
    TEF_CHECK(repository.value()->close().ok());
  }
}

TEF_TEST(a_corrupt_journal_record_stops_replay_at_the_last_valid_prefix) {
  const std::string directory = tef::test::temporary_directory("corrupt-journal");
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    for (std::uint64_t i = 1; i <= 6; ++i) {
      TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(i, "entry"))).ok());
    }
    TEF_CHECK(repository.value()->close().ok());
  }

  const std::filesystem::path journal = std::filesystem::path(directory) / "journal.tefj";
  std::vector<std::byte> bytes = read_file(journal);
  TEF_CHECK(bytes.size() > 120);
  // Corrupt a byte inside the fourth record's payload range.
  bytes[110] = static_cast<std::byte>(static_cast<unsigned>(bytes[110]) ^ 0xFFu);
  write_file(journal, bytes);

  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    TEF_CHECK_LT(repository.value()->record_count(), std::size_t{6});
    TEF_CHECK(repository.value()->recovery().journal_bytes_truncated > 0);
    TEF_CHECK(repository.value()->close().ok());
  }
}

TEF_TEST(a_corrupt_snapshot_is_refused_rather_than_silently_ignored) {
  const std::string directory = tef::test::temporary_directory("corrupt-snapshot");
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    for (std::uint64_t i = 1; i <= 4; ++i) {
      TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(i, "entry"))).ok());
    }
    TEF_CHECK(repository.value()->compact().ok());
    TEF_CHECK(repository.value()->close().ok());
  }

  const std::filesystem::path snapshot = std::filesystem::path(directory) / "snapshot.tefs";
  std::vector<std::byte> bytes = read_file(snapshot);
  TEF_CHECK(!bytes.empty());
  bytes[bytes.size() / 2] = static_cast<std::byte>(static_cast<unsigned>(bytes[bytes.size() / 2]) ^ 0x11u);
  write_file(snapshot, bytes);

  Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
  TEF_CHECK(!repository.has_value());
  TEF_CHECK_EQ(repository.error().code(), ErrorCode::integrity_failure);
}

TEF_TEST(an_interrupted_atomic_replacement_is_discarded_on_open) {
  const std::string directory = tef::test::temporary_directory("temp-files");
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    for (std::uint64_t i = 1; i <= 5; ++i) {
      TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(i, "entry"))).ok());
    }
    TEF_CHECK(repository.value()->close().ok());
  }
  const std::filesystem::path snapshot_temp = std::filesystem::path(directory) / "snapshot.tefs.tmp";
  const std::filesystem::path journal_temp = std::filesystem::path(directory) / "journal.tefj.tmp";
  write_file(snapshot_temp, {std::byte{1}, std::byte{2}, std::byte{3}});
  write_file(journal_temp, {std::byte{9}});

  Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
  TEF_CHECK(repository.has_value());
  TEF_CHECK(!std::filesystem::exists(snapshot_temp));
  TEF_CHECK(!std::filesystem::exists(journal_temp));
  TEF_CHECK_EQ(repository.value()->record_count(), std::size_t{5});
  TEF_CHECK(repository.value()->close().ok());
}

TEF_TEST(compaction_preserves_every_record_and_shrinks_the_journal) {
  const std::string directory = tef::test::temporary_directory("compaction");
  const std::filesystem::path journal = std::filesystem::path(directory) / "journal.tefj";
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory, 8));
    TEF_CHECK(repository.has_value());
    for (std::uint64_t i = 1; i <= 40; ++i) {
      TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(i, "entry"))).ok());
    }
    TEF_CHECK(repository.value()->counters().compactions >= 1);
    TEF_CHECK_EQ(repository.value()->record_count(), std::size_t{40});
    TEF_CHECK(repository.value()->close().ok());
  }
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory, 8));
    TEF_CHECK(repository.has_value());
    TEF_CHECK(repository.value()->recovery().snapshot_loaded);
    TEF_CHECK_EQ(repository.value()->recovery().snapshot_sequence, std::uint64_t{40});
    TEF_CHECK_EQ(repository.value()->record_count(), std::size_t{40});
    const std::vector<DurableRecord> records = repository.value()->records();
    for (std::size_t i = 0; i < records.size(); ++i) {
      TEF_CHECK_EQ(records[i].sequence, static_cast<std::uint64_t>(i) + 1);
      const Result<AuditRecord> decoded = decode_audit(records[i].payload);
      TEF_CHECK(decoded.has_value());
      TEF_CHECK_EQ(decoded.value().tick, static_cast<std::uint64_t>(i) + 1);
    }
    TEF_CHECK(repository.value()->close().ok());
  }
  TEF_CHECK(journal_size(journal) < 4096);
}

TEF_TEST(coordinator_epoch_and_fences_are_durable_and_the_epoch_advances) {
  const std::string directory = tef::test::temporary_directory("epoch-fence");
  const PublisherId publisher = PublisherId::parse("publisher-one").value();
  const BootId dead_boot = derive_boot_id(11, 22);
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    TEF_CHECK_EQ(repository.value()->coordinator_epoch(), std::uint64_t{0});
    CoordinatorEpochRecord epoch;
    epoch.coordinator = CoordinatorId::parse("tef-coordinator").value();
    epoch.incarnation = CoordinatorIncarnation::first();
    epoch.boot = derive_boot_id(1, 2);
    TEF_CHECK(repository.value()->set_coordinator_epoch(1, epoch).ok());
    FenceRecord fence;
    fence.publisher = publisher;
    fence.boot = dead_boot;
    fence.incarnation = CoordinatorIncarnation::first();
    fence.reason = "test fence";
    fence.tick = 1;
    TEF_CHECK(repository.value()->add_fence(fence).ok());
    TEF_CHECK(repository.value()->close().ok());
  }
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    const std::uint64_t previous = repository.value()->coordinator_epoch();
    TEF_CHECK_EQ(previous, std::uint64_t{1});
    CoordinatorEpochRecord epoch;
    epoch.coordinator = CoordinatorId::parse("tef-coordinator").value();
    epoch.incarnation = CoordinatorIncarnation::parse(2).value();
    epoch.boot = derive_boot_id(3, 4);
    TEF_CHECK(repository.value()->set_coordinator_epoch(previous + 1, epoch).ok());
    const std::vector<FenceRecord> fences = repository.value()->fences();
    TEF_CHECK_EQ(fences.size(), std::size_t{1});
    TEF_CHECK(fences[0].boot == dead_boot);
    TEF_CHECK(fences[0].publisher == publisher);
    TEF_CHECK(repository.value()->close().ok());
  }
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    TEF_CHECK_EQ(repository.value()->coordinator_epoch(), std::uint64_t{2});
    TEF_CHECK_EQ(repository.value()->fences().size(), std::size_t{1});
    TEF_CHECK(repository.value()->close().ok());
  }
}

TEF_TEST(the_record_bound_is_enforced) {
  const std::string directory = tef::test::temporary_directory("record-bound");
  PlanRepository::Options options = options_for(directory);
  options.max_records = 4;
  Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options);
  TEF_CHECK(repository.has_value());
  for (std::uint64_t i = 1; i <= 4; ++i) {
    TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(i, "entry"))).ok());
  }
  const Status overflow = repository.value()->append(RecordType::audit, encode_audit(audit(5, "entry")));
  TEF_CHECK(!overflow.ok());
  TEF_CHECK_EQ(overflow.code(), ErrorCode::limit_exceeded);
  TEF_CHECK_EQ(repository.value()->record_count(), std::size_t{4});
  TEF_CHECK(repository.value()->close().ok());
}

TEF_TEST(plan_records_round_trip_through_the_durable_format) {
  Engine engine;
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());
  TEF_CHECK(engine.submit_snapshot(snapshot).ok());
  DeclareRequest request;
  request.plan_id = PlanId::parse("plan-durable-record").value();
  request.attempt = AttemptId::parse("attempt-durable-record").value();
  request.attempt_generation = AttemptGeneration::first();
  request.publisher = PublisherId::parse("publisher-one").value();
  request.publisher_boot = derive_boot_id(5, 5);
  const Result<Plan> declared = engine.declare(request);
  TEF_CHECK_MSG(declared.has_value(), declared.has_value() ? "" : declared.error().format());
  TEF_CHECK(engine.validate(declared.value().ref()).ok());
  const Result<Plan> solved = engine.solve(declared.value().ref());
  TEF_CHECK(solved.has_value());
  TEF_CHECK(engine.authorize(solved.value().ref(), AuthorizeRequest{}).has_value());
  CommitIntent intent;
  intent.commit = CommitId::parse("commit-durable-record").value();
  intent.commit_generation = CommitGeneration::first();
  const Result<Plan> committed = engine.commit(solved.value().ref(), intent);
  TEF_CHECK(committed.has_value());

  const std::vector<std::byte> payload = encode_plan(committed.value());
  const Result<Plan> decoded = decode_plan(std::span<const std::byte>(payload.data(), payload.size()));
  TEF_CHECK_MSG(decoded.has_value(), decoded.has_value() ? "" : decoded.error().format());
  TEF_CHECK_EQ(decoded.value().content_digest().hex(), committed.value().content_digest().hex());
  TEF_CHECK_EQ(decoded.value().authority.digest().hex(), committed.value().authority.digest().hex());
  TEF_CHECK_EQ(decoded.value().allocation.digest().hex(), committed.value().allocation.digest().hex());
  TEF_CHECK_EQ(decoded.value().state, PlanState::committed);
  TEF_CHECK_EQ(decoded.value().feasibility.binding.size(), committed.value().feasibility.binding.size());

  for (std::size_t cut = 0; cut < payload.size(); cut += 11) {
    const Result<Plan> truncated = decode_plan(std::span<const std::byte>(payload.data(), cut));
    TEF_CHECK(!truncated.has_value());
  }
  std::vector<std::byte> trailing = payload;
  trailing.push_back(std::byte{0});
  TEF_CHECK(!decode_plan(std::span<const std::byte>(trailing.data(), trailing.size())).has_value());

  std::vector<std::byte> zeroed = payload;
  zeroed[40] = std::byte{0};
  zeroed[41] = std::byte{0};
  const Result<Plan> damaged = decode_plan(std::span<const std::byte>(zeroed.data(), zeroed.size()));
  TEF_CHECK(!damaged.has_value());
}

TEF_TEST(an_engine_restart_preserves_supersession_lineage) {
  const std::string directory = tef::test::temporary_directory("restart-lineage");
  const FabricSnapshot snapshot = build_snapshot(simple_fabric());

  const auto commit_plan = [&](Engine& engine, const char* plan_name, const char* commit_name,
                               std::uint64_t generation) -> Plan {
    DeclareRequest request;
    request.plan_id = PlanId::parse(plan_name).value();
    request.attempt = AttemptId::parse(std::string("attempt-") + plan_name).value();
    request.attempt_generation = AttemptGeneration::first();
    request.publisher = PublisherId::parse("publisher-one").value();
    request.publisher_boot = derive_boot_id(9, 9);
    const Result<Plan> declared = engine.declare(request);
    TEF_CHECK(declared.has_value());
    TEF_CHECK(engine.validate(declared.value().ref()).ok());
    const Result<Plan> solved = engine.solve(declared.value().ref());
    TEF_CHECK(solved.has_value());
    TEF_CHECK(engine.authorize(solved.value().ref(), AuthorizeRequest{}).has_value());
    CommitIntent intent;
    intent.commit = CommitId::parse(commit_name).value();
    intent.commit_generation = CommitGeneration::parse(generation).value();
    const Result<Plan> committed = engine.commit(solved.value().ref(), intent);
    TEF_CHECK(committed.has_value());
    return committed.value();
  };

  {
    Engine engine;
    Result<std::unique_ptr<PlanRepository>> repository =
        PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    std::shared_ptr<PlanRepository> shared(std::move(repository.value()));
    TEF_CHECK(engine.attach_repository(shared).ok());
    TEF_CHECK(engine.submit_snapshot(snapshot).ok());
    const Plan first = commit_plan(engine, "plan-1", "commit-1", 1);
    TEF_CHECK_EQ(first.state, PlanState::committed);
    TEF_CHECK(shared->close().ok());
  }

  {
    Engine engine;
    Result<std::unique_ptr<PlanRepository>> repository =
        PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    std::shared_ptr<PlanRepository> shared(std::move(repository.value()));
    TEF_CHECK(engine.attach_repository(shared).ok());
    TEF_CHECK(engine.recover_from_repository().ok());
    const auto restored = engine.get(PlanId::parse("plan-1").value());
    TEF_CHECK(restored.has_value());
    TEF_CHECK_EQ(restored->state, PlanState::committed);
    TEF_CHECK_EQ(restored->commit.str(), std::string("commit-1"));
    TEF_CHECK_EQ(restored->applicability, PlanApplicability::revalidation_required);

    TEF_CHECK(engine.submit_snapshot(snapshot).ok());
    TEF_CHECK_EQ(engine.revalidate(restored->ref()).value(), PlanApplicability::current);

    const Plan second = commit_plan(engine, "plan-2", "commit-2", 2);
    TEF_CHECK_EQ(second.supersedes.id.str(), std::string("plan-1"));
    const auto previous = engine.get(PlanId::parse("plan-1").value());
    TEF_CHECK(previous.has_value());
    TEF_CHECK_EQ(previous->state, PlanState::superseded);
    TEF_CHECK(shared->close().ok());
  }

  {
    Engine engine;
    Result<std::unique_ptr<PlanRepository>> repository =
        PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    std::shared_ptr<PlanRepository> shared(std::move(repository.value()));
    TEF_CHECK(engine.attach_repository(shared).ok());
    TEF_CHECK(engine.recover_from_repository().ok());
    const auto first = engine.get(PlanId::parse("plan-1").value());
    const auto second = engine.get(PlanId::parse("plan-2").value());
    TEF_CHECK(first.has_value());
    TEF_CHECK(second.has_value());
    TEF_CHECK_EQ(first->state, PlanState::superseded);
    TEF_CHECK_EQ(second->state, PlanState::committed);
    TEF_CHECK_EQ(first->superseded_by.id.str(), std::string("plan-2"));
    TEF_CHECK_EQ(second->supersedes.id.str(), std::string("plan-1"));
    TEF_CHECK(shared->close().ok());
  }
}

TEF_TEST(a_garbage_journal_file_is_refused) {
  const std::string directory = tef::test::temporary_directory("garbage-journal");
  {
    Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
    TEF_CHECK(repository.has_value());
    TEF_CHECK(repository.value()->append(RecordType::audit, encode_audit(audit(1, "entry"))).ok());
    TEF_CHECK(repository.value()->close().ok());
  }
  const std::filesystem::path journal = std::filesystem::path(directory) / "journal.tefj";
  std::vector<std::byte> bytes(read_file(journal));
  for (auto& byte : bytes) byte = std::byte{0x5A};
  write_file(journal, bytes);

  Result<std::unique_ptr<PlanRepository>> repository = PlanRepository::open(options_for(directory));
  TEF_CHECK(!repository.has_value());
  TEF_CHECK_EQ(repository.error().code(), ErrorCode::integrity_failure);
}

int main(int argc, char** argv) { return tef::test::run_all(argc, argv); }
