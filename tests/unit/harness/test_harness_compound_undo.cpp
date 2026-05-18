// Tests for Harness::compoundRewind.
//
// Goals:
//   * Lock down the contract: rewinding to a turn discards every turn
//     after it AND every edit batch authored from those turns.
//   * Lock down atomicity: if any batch is blocked, NOTHING is changed.
//   * Lock down compound linkage: the resulting TranscriptUndoAction
//     must carry the EditUndoAction ids for the batches we undid, so
//     a future compound redo can replay them in order.
//
// We work directly through ThreadManager + Engine + a MockAgent so the
// test stays small. The same harness layer the daemon hits in production
// is exercised — just without the full Engine init dance.

#include <gtest/gtest.h>

#include "Engine.hpp"
#include "harness/Harness.hpp"
#include "AgentRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include "Context.hpp"
#include "MockAgent.hpp"
#include "MockEnvironment.hpp"
#include "utils/Hashline.hpp"

#include <filesystem>
#include <cstdlib>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

class CompoundRewindTest : public ::testing::Test {
protected:
  std::string tempDir;
  std::string originalHome;
  std::string threadId;
  std::shared_ptr<firmius::test::MockAgent> agent;
  std::shared_ptr<firmius::test::MockEnvironment> env;

  void SetUp() override {
    char tempTemplate[] = "/tmp/firmius_compound_undo_XXXXXX";
    char *result = mkdtemp(tempTemplate);
    ASSERT_NE(result, nullptr);
    tempDir = result;

    originalHome = getenv("HOME") ? std::string(getenv("HOME")) : "";
    setenv("HOME", tempDir.c_str(), 1);
    std::filesystem::create_directories(tempDir + "/.firmius/threads");

    ThreadManager tm(tempDir + "/.firmius/threads");
    ThreadMetadata metadata;
    metadata.title = "rewind";
    metadata.hostOptions.type = HostType::Local;
    threadId = tm.createThread(metadata);

    AgentContext context;
    context.history = std::make_shared<AgentHistory>();
    context.history->threadId = threadId;
    context.identity.id = "agent-1";
    env = std::make_shared<firmius::test::MockEnvironment>();
    agent = std::make_shared<firmius::test::MockAgent>(context, env);
    AgentRegistry::instance().registerAgent("agent-1", agent);
  }

  void TearDown() override {
    for (const auto &id : AgentRegistry::instance().listAll()) {
      AgentRegistry::instance().unregisterAgent(id);
    }
    if (!originalHome.empty()) setenv("HOME", originalHome.c_str(), 1);
    else unsetenv("HOME");
    std::filesystem::remove_all(tempDir);
  }

  // ── Helpers that mutate the in-memory agent + persisted batches ──

  /// Append a turn with a single user message (or assistant — caller picks
  /// via the role argument). Returns the turn id.
  std::string appendTurn(Role role, const std::string &text,
                         std::uint64_t timestamp) {
    AgentTurn turn;
    turn.turnId = "turn-" + std::to_string(agent->getMutableContext().history->turns.size() + 1);
    Message msg;
    msg.id = turn.turnId + "-msg";
    msg.role = role;
    msg.timestamp = timestamp;
    msg.content.push_back(TextContent{text});
    turn.messages.push_back(std::move(msg));
    agent->getMutableContext().history->turns.push_back(std::move(turn));
    return agent->getMutableContext().history->turns.back().turnId;
  }

  /// Persist an edit batch attached to the given turn. Writes a single
  /// trivial file mutation so the eligibility evaluator sees a concrete
  /// batch on disk.
  std::string addPersistedEditBatch(const std::string &turnId,
                                    const std::string &fileLabel,
                                    std::uint64_t createdAt) {
    ThreadManager tm(tempDir + "/.firmius/threads");
    EditBatchSummary summary;
    summary.editBatchId = "edit-" + turnId + "-" + fileLabel;
    summary.threadId = threadId;
    summary.agentId = "agent-1";
    summary.toolName = "EditWrite";
    summary.summaryText = "edited " + fileLabel;
    summary.status = EditBatchStatus::Applied;
    summary.files = {fileLabel};
    summary.turnId = turnId;
    summary.createdAt = createdAt;
    summary.addedLines = 2;
    summary.removedLines = 1;

    EditFileMutation mutation;
    mutation.fileMutationId = summary.editBatchId + "-mut";
    mutation.editBatchId = summary.editBatchId;
    mutation.threadId = threadId;
    mutation.filePath = fileLabel;
    mutation.hadFileBefore = true;
    mutation.hasFileAfter = true;
    mutation.status = EditFileMutationStatus::Applied;
    mutation.operations.push_back(EditMutationOperation{
        "replace line", 1, 1, {"old"}, {"new"}});

    tm.writeEditBatch(threadId, summary, {mutation});
    return summary.editBatchId;
  }

  /// Persist an edit batch that the engine cannot undo. We use the
  /// "create" shape (hadFileBefore = false) which Engine::evaluate
  /// flags as RejectedBatchNotFullyUndoable. Tests use this to seed
  /// "the rewind preflight must abort here" scenarios.
  std::string addBlockedEditBatch(const std::string &turnId,
                                   const std::string &fileLabel,
                                   std::uint64_t createdAt) {
    ThreadManager tm(tempDir + "/.firmius/threads");
    EditBatchSummary summary;
    summary.editBatchId = "edit-blocked-" + turnId + "-" + fileLabel;
    summary.threadId = threadId;
    summary.agentId = "agent-1";
    summary.toolName = "FileWrite";  // creation
    summary.summaryText = "created " + fileLabel;
    summary.status = EditBatchStatus::Applied;
    summary.files = {fileLabel};
    summary.turnId = turnId;
    summary.createdAt = createdAt;
    summary.addedLines = 5;
    summary.removedLines = 0;

    EditFileMutation mutation;
    mutation.fileMutationId = summary.editBatchId + "-mut";
    mutation.editBatchId = summary.editBatchId;
    mutation.threadId = threadId;
    mutation.filePath = fileLabel;
    mutation.hadFileBefore = false;  // file did not exist before
    mutation.hasFileAfter = true;
    mutation.status = EditFileMutationStatus::Applied;

    tm.writeEditBatch(threadId, summary, {mutation});
    return summary.editBatchId;
  }

  /// Persist a create-style edit batch and write the file to disk
  /// matching the recorded post-image. Used by the new create-undo /
  /// redo / chain tests. Returns the editBatchId.
  std::string addCreateBatchWithDiskFile(const std::string &turnId,
                                         const std::string &fileLabel,
                                         const std::string &content,
                                         std::uint64_t createdAt) {
    ThreadManager tm(tempDir + "/.firmius/threads");

    // Compute the same fingerprint Agent.cpp used so eligibility's
    // disk-tamper check passes. The recorder built afterContent by
    // joining op.newLines with '\n', and computeContentFingerprint =
    // Hashline::computeHash(content) + "-" + size. We reproduce that
    // here exactly.
    auto splitLines = [](const std::string &s) {
      std::vector<std::string> out;
      std::string cur;
      for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
      }
      if (!cur.empty() || s.empty() || s.back() == '\n') {
        // Match split behavior — trailing newline produces a trailing
        // entry only if there was content; we keep cur if non-empty.
        if (!cur.empty()) out.push_back(cur);
      }
      return out;
    };
    auto join = [](const std::vector<std::string> &lines) {
      std::string out;
      for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n";
        out += lines[i];
      }
      return out;
    };
    const auto lines = splitLines(content);
    const std::string normalized = join(lines);
    const std::string postHash =
        firmius::shared::utils::Hashline::computeHash(normalized) + "-" +
        std::to_string(normalized.size());

    EditBatchSummary summary;
    summary.editBatchId = "edit-create-" + turnId + "-" + fileLabel;
    summary.threadId = threadId;
    summary.agentId = "agent-1";
    summary.toolName = "FileWrite";
    summary.summaryText = "created " + fileLabel;
    summary.status = EditBatchStatus::Applied;
    summary.files = {fileLabel};
    summary.turnId = turnId;
    summary.createdAt = createdAt;
    summary.addedLines = static_cast<int>(lines.size());

    EditFileMutation mutation;
    mutation.fileMutationId = summary.editBatchId + "-mut";
    mutation.editBatchId = summary.editBatchId;
    mutation.threadId = threadId;
    mutation.filePath = fileLabel;
    mutation.hadFileBefore = false;
    mutation.hasFileAfter = true;
    mutation.postSize = normalized.size();
    mutation.postHash = postHash;
    mutation.status = EditFileMutationStatus::Applied;
    mutation.operations.push_back(EditMutationOperation{
        "create file", 1, 0, {}, lines});

    tm.writeEditBatch(threadId, summary, {mutation});

    // Write the file to disk via the mock host and wire path resolution.
    auto host = std::dynamic_pointer_cast<firmius::test::MockHost>(env->getHost());
    if (host) {
      const std::string absolutePath = tempDir + "/workspace/" + fileLabel;
      host->writeFile(absolutePath,
                      std::vector<uint8_t>(content.begin(), content.end()));
      ON_CALL(env->mockWorkspace(), resolvePath(fileLabel))
          .WillByDefault(testing::Return(absolutePath));
      ON_CALL(env->mockWorkspace(), recordFileEdit(absolutePath))
          .WillByDefault(testing::Return());
    }
    return summary.editBatchId;
  }

  /// Force an edit batch into Undone state on disk, so eligibility flips
  /// to RejectedAlreadyUndone. Used to seed "blocked batch" scenarios.
  void markEditBatchUndone(const std::string &editBatchId) {
    ThreadManager tm(tempDir + "/.firmius/threads");
    auto detail = tm.getEditBatch(threadId, editBatchId);
    detail.summary.status = EditBatchStatus::Undone;
    for (auto &m : detail.files) {
      m.status = EditFileMutationStatus::Undone;
    }
    tm.writeEditBatch(threadId, detail.summary, detail.files);
  }
};

// ── Tests ────────────────────────────────────────────────────────────────

// Conversation-only mode discards transcript turns and leaves edit batches
// untouched. We add 3 turns, attach an edit batch to turn 2, rewind to
// turn 2 (which keeps turn 1 and discards 2+3), and assert the batch's
// persisted Applied status didn't change.
TEST_F(CompoundRewindTest, RestoreConversationLeavesEditBatchesAlone) {
  appendTurn(Role::User, "first", 1000);
  const std::string t2 = appendTurn(Role::User, "second", 2000);
  appendTurn(Role::Assistant, "third", 3000);
  const std::string batchId = addPersistedEditBatch(t2, "a.txt", 2500);

  auto result = Harness::instance().compoundRewind(
      threadId, "agent-1", t2, Harness::CompoundRewindMode::RestoreConversation);

  EXPECT_TRUE(result.applied);
  EXPECT_EQ(result.turnsUndone, 2);  // turn 2 + turn 3 are both > target
  EXPECT_TRUE(result.editUndoActionIds.empty());

  ThreadManager tm(tempDir + "/.firmius/threads");
  auto persisted = tm.getEditBatch(threadId, batchId);
  EXPECT_EQ(persisted.summary.status, EditBatchStatus::Applied)
      << "RestoreConversation must not touch persisted edit batches";
}

// Code-only mode undoes edit batches and leaves the transcript intact.
TEST_F(CompoundRewindTest, RestoreCodeLeavesTranscriptIntact) {
  appendTurn(Role::User, "first", 1000);
  const std::string t2 = appendTurn(Role::User, "second", 2000);
  const std::string batchId = addPersistedEditBatch(t2, "b.txt", 2500);

  // Wire up workspace path resolution so undoEditBatch's file restoration
  // doesn't blow up looking up paths.
  auto host = std::dynamic_pointer_cast<firmius::test::MockHost>(env->getHost());
  ASSERT_TRUE(host);
  const std::string absolutePath = tempDir + "/workspace/b.txt";
  host->writeFile(absolutePath, std::vector<uint8_t>{'n', 'e', 'w'});
  EXPECT_CALL(env->mockWorkspace(), resolvePath("b.txt"))
      .WillRepeatedly(testing::Return(absolutePath));
  EXPECT_CALL(env->mockWorkspace(), recordFileEdit(absolutePath))
      .Times(testing::AtLeast(1));

  const auto turnsBefore = agent->getMutableContext().history->turns.size();

  auto result = Harness::instance().compoundRewind(
      threadId, "agent-1", t2, Harness::CompoundRewindMode::RestoreCode);

  EXPECT_TRUE(result.applied);
  // Code-only rewind DOES produce a TranscriptUndoAction now — without
  // it, /redo's picker has no row to surface for code-only undos and
  // the user can never get their files back via the redo flow.
  // The action's redo payload is empty (no captured turns) but its
  // editUndoActionIds list lets compoundRedo replay the edit redos.
  EXPECT_FALSE(result.undoActionId.empty())
      << "RestoreCode must persist a TranscriptUndoAction so /redo can find the linked edit-undo actions";
  EXPECT_EQ(result.turnsUndone, 0)
      << "RestoreCode must NOT truncate the transcript — turnsUndone should stay 0";
  EXPECT_EQ(result.editUndoActionIds.size(), 1u);
  EXPECT_EQ(agent->getMutableContext().history->turns.size(), turnsBefore)
      << "RestoreCode must not modify the in-memory transcript";

  // Verify the persisted action has the right shape: empty redo payload,
  // populated editUndoActionIds.
  ThreadManager tm(tempDir + "/.firmius/threads");
  auto stored = tm.findTranscriptUndoAction(threadId, result.undoActionId);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->scopeType, "edits_only");
  EXPECT_TRUE(stored->redoAvailable);
  EXPECT_EQ(stored->editUndoActionIds.size(), 1u);
  auto payloads = tm.loadTranscriptRedoPayloads(threadId, result.undoActionId);
  EXPECT_TRUE(payloads.empty())
      << "Code-only undo captures no turns, so the redo payload list must be empty";
}

// Compound mode undoes edit batches first (newest first), THEN the
// transcript turns. The resulting TranscriptUndoAction must carry the
// edit-undo action ids so a future compound redo can replay them.
TEST_F(CompoundRewindTest, RestoreCodeAndConversationLinksUndoActions) {
  appendTurn(Role::User, "first", 1000);
  const std::string t2 = appendTurn(Role::User, "second", 2000);
  const std::string batchA = addPersistedEditBatch(t2, "alpha.txt", 2100);
  const std::string batchB = addPersistedEditBatch(t2, "beta.txt", 2200);

  auto host = std::dynamic_pointer_cast<firmius::test::MockHost>(env->getHost());
  ASSERT_TRUE(host);
  for (const auto &name : {"alpha.txt", "beta.txt"}) {
    const std::string abs = tempDir + "/workspace/" + name;
    host->writeFile(abs, std::vector<uint8_t>{'n', 'e', 'w'});
    EXPECT_CALL(env->mockWorkspace(), resolvePath(std::string(name)))
        .WillRepeatedly(testing::Return(abs));
    EXPECT_CALL(env->mockWorkspace(), recordFileEdit(abs))
        .Times(testing::AtLeast(1));
  }

  auto result = Harness::instance().compoundRewind(
      threadId, "agent-1", t2,
      Harness::CompoundRewindMode::RestoreCodeAndConversation);

  EXPECT_TRUE(result.applied);
  EXPECT_FALSE(result.undoActionId.empty());
  ASSERT_EQ(result.editUndoActionIds.size(), 2u)
      << "Both edit batches authored at/after target turn should be undone";

  // The persisted TranscriptUndoAction should have the same compound link
  // we returned, so a future redo can find them.
  ThreadManager tm(tempDir + "/.firmius/threads");
  auto stored = tm.findTranscriptUndoAction(threadId, result.undoActionId);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->editUndoActionIds, result.editUndoActionIds)
      << "TranscriptUndoAction must persist editUndoActionIds for compound redo";
}

// If any edit batch we'd need to undo is not undoable (here, a "create"
// batch — Engine flags those as RejectedBatchNotFullyUndoable), the
// pre-flight must abort BEFORE applying any change. The transcript must
// still have all its turns and the OTHER batch must still be Applied.
TEST_F(CompoundRewindTest, AbortsAtomicallyWhenAnyEditBatchBlocked) {
  appendTurn(Role::User, "first", 1000);
  const std::string t2 = appendTurn(Role::User, "second", 2000);
  // batchA is a "create" batch — Engine refuses to undo it.
  const std::string batchA = addBlockedEditBatch(t2, "alpha.txt", 2100);
  // batchB is a normal modification — undoable on its own.
  const std::string batchB = addPersistedEditBatch(t2, "beta.txt", 2200);

  const auto turnsBefore = agent->getMutableContext().history->turns.size();

  auto result = Harness::instance().compoundRewind(
      threadId, "agent-1", t2,
      Harness::CompoundRewindMode::RestoreCodeAndConversation);

  EXPECT_FALSE(result.applied);
  EXPECT_FALSE(result.errorMessage.empty());
  EXPECT_TRUE(result.editUndoActionIds.empty());
  EXPECT_TRUE(result.undoActionId.empty());

  // Transcript must be untouched.
  EXPECT_EQ(agent->getMutableContext().history->turns.size(), turnsBefore);

  // Both batches must still be Applied — pre-flight rejected before
  // touching either.
  ThreadManager tm(tempDir + "/.firmius/threads");
  EXPECT_EQ(tm.getEditBatch(threadId, batchA).summary.status, EditBatchStatus::Applied);
  EXPECT_EQ(tm.getEditBatch(threadId, batchB).summary.status, EditBatchStatus::Applied)
      << "The unblocked batch must NOT be undone when atomicity fails";
}

// Rewinding to an unknown turn id is a clean error — no state change.
TEST_F(CompoundRewindTest, UnknownTurnIdReturnsError) {
  appendTurn(Role::User, "first", 1000);

  auto result = Harness::instance().compoundRewind(
      threadId, "agent-1", "definitely-not-a-real-turn",
      Harness::CompoundRewindMode::RestoreCodeAndConversation);

  EXPECT_FALSE(result.applied);
  EXPECT_FALSE(result.errorMessage.empty());
}

// ── Create-style undo / redo ─────────────────────────────────────────────

// Undoing a "create file" batch must DELETE the file. The pre-fix
// behavior wrote empty content and left the file on disk, which is
// exactly the bug the user reported.
TEST_F(CompoundRewindTest, RestoreCodeUndoesFileCreations) {
  appendTurn(Role::User, "first", 1000);
  const std::string t2 = appendTurn(Role::User, "second", 2000);
  const std::string batch =
      addCreateBatchWithDiskFile(t2, "newfile.txt", "hello\nworld", 2100);

  auto host = std::dynamic_pointer_cast<firmius::test::MockHost>(env->getHost());
  ASSERT_TRUE(host);
  const std::string absolutePath = tempDir + "/workspace/newfile.txt";
  ASSERT_TRUE(host->exists(absolutePath))
      << "test setup: file should exist on disk before undo";

  auto result = Harness::instance().compoundRewind(
      threadId, "agent-1", t2, Harness::CompoundRewindMode::RestoreCode);

  EXPECT_TRUE(result.applied) << result.errorMessage;
  EXPECT_EQ(result.editUndoActionIds.size(), 1u);
  EXPECT_FALSE(host->exists(absolutePath))
      << "create-style undo must delete the file from disk";
}

// Create + overwrite of the SAME file in the same turn target. Pre-fix,
// the eligibility check rejected the create because a "later edit"
// (the overwrite) was still applied — but both batches were in the
// rewind set and would unwind together. coUndoBatchIds fixes this.
TEST_F(CompoundRewindTest, RestoreCodeAndConversationChainSameFile) {
  appendTurn(Role::User, "first", 1000);
  const std::string t2 = appendTurn(Role::User, "second", 2000);
  // Same file edited twice in the same turn: first create, then overwrite.
  const std::string createBatch =
      addCreateBatchWithDiskFile(t2, "chained.txt", "v1", 2100);
  // Overwrite the file: write "v2" to disk and persist a normal
  // line-edit batch with the SAME postHash basis as v2's normalized
  // content.
  auto host = std::dynamic_pointer_cast<firmius::test::MockHost>(env->getHost());
  ASSERT_TRUE(host);
  const std::string absolutePath = tempDir + "/workspace/chained.txt";
  host->writeFile(absolutePath, std::vector<uint8_t>{'v', '2'});

  ThreadManager tm(tempDir + "/.firmius/threads");
  EditBatchSummary overwriteSummary;
  overwriteSummary.editBatchId = "edit-overwrite-" + t2 + "-chained";
  overwriteSummary.threadId = threadId;
  overwriteSummary.agentId = "agent-1";
  overwriteSummary.toolName = "EditWrite";
  overwriteSummary.summaryText = "overwrite chained.txt";
  overwriteSummary.status = EditBatchStatus::Applied;
  overwriteSummary.files = {"chained.txt"};
  overwriteSummary.turnId = t2;
  overwriteSummary.createdAt = 2200;  // newer than create
  overwriteSummary.addedLines = 1;
  overwriteSummary.removedLines = 1;
  EditFileMutation overwriteMut;
  overwriteMut.fileMutationId = overwriteSummary.editBatchId + "-mut";
  overwriteMut.editBatchId = overwriteSummary.editBatchId;
  overwriteMut.threadId = threadId;
  overwriteMut.filePath = "chained.txt";
  overwriteMut.hadFileBefore = true;
  overwriteMut.hasFileAfter = true;
  overwriteMut.status = EditFileMutationStatus::Applied;
  overwriteMut.operations.push_back(EditMutationOperation{
      "overwrite file", 1, 1, {"v1"}, {"v2"}});
  tm.writeEditBatch(threadId, overwriteSummary, {overwriteMut});

  auto result = Harness::instance().compoundRewind(
      threadId, "agent-1", t2,
      Harness::CompoundRewindMode::RestoreCodeAndConversation);

  EXPECT_TRUE(result.applied) << result.errorMessage;
  EXPECT_EQ(result.editUndoActionIds.size(), 2u)
      << "Both batches in the chain must unwind together";
  EXPECT_FALSE(host->exists(absolutePath))
      << "The original create unwound, so the file should be gone";
}

// If the user modifies a created file after the agent created it, undo
// must refuse rather than silently delete the user's work.
TEST_F(CompoundRewindTest, EligibilityRejectsTamperedCreatedFile) {
  appendTurn(Role::User, "first", 1000);
  const std::string t2 = appendTurn(Role::User, "second", 2000);
  const std::string batch =
      addCreateBatchWithDiskFile(t2, "tampered.txt", "original", 2100);

  // User externally modifies the file — different content, same path.
  auto host = std::dynamic_pointer_cast<firmius::test::MockHost>(env->getHost());
  ASSERT_TRUE(host);
  const std::string absolutePath = tempDir + "/workspace/tampered.txt";
  host->writeFile(absolutePath,
                  std::vector<uint8_t>{'u', 's', 'e', 'r', '\'', 's'});

  auto result = Harness::instance().compoundRewind(
      threadId, "agent-1", t2, Harness::CompoundRewindMode::RestoreCode);

  // Eligibility passes (postHash is set), but undoEditBatch's disk
  // fingerprint check refuses, so result.applied is false from the
  // undo step — partial-rewind error surfaces in errorMessage.
  EXPECT_FALSE(result.applied);
  EXPECT_FALSE(result.errorMessage.empty());
  EXPECT_TRUE(host->exists(absolutePath))
      << "User-modified file must NOT be deleted by undo";
}

}  // namespace
