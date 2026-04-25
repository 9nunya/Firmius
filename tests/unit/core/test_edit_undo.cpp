#include <gtest/gtest.h>

#include "Engine.hpp"
#include "AgentRegistry.hpp"
#include "persistence/ThreadManager.hpp"
#include "Context.hpp"
#include "MockAgent.hpp"
#include "MockEnvironment.hpp"

#include <filesystem>
#include <cstdlib>

using namespace firmius::core;
using namespace firmius::shared;

namespace {

class EditUndoSmokeTest : public ::testing::Test {
protected:
    std::string tempDir;
    std::string originalHome;

    void SetUp() override {
        char tempTemplate[] = "/tmp/firmius_edit_undo_XXXXXX";
        char* result = mkdtemp(tempTemplate);
        ASSERT_NE(result, nullptr);
        tempDir = result;

        originalHome = getenv("HOME") ? std::string(getenv("HOME")) : "";
        setenv("HOME", tempDir.c_str(), 1);
        std::filesystem::create_directories(tempDir + "/.firmius/threads");
    }

    void TearDown() override {
        for (const auto &agentId : AgentRegistry::instance().listAll()) {
            AgentRegistry::instance().unregisterAgent(agentId);
        }
        if (!originalHome.empty()) setenv("HOME", originalHome.c_str(), 1);
        else unsetenv("HOME");
        std::filesystem::remove_all(tempDir);
    }
};

TEST_F(EditUndoSmokeTest, EvaluateUndoRejectsUndoneBatch) {
    ThreadManager tm(tempDir + "/.firmius/threads");
    ThreadMetadata metadata;
    metadata.title = "undo";
    metadata.hostOptions.type = HostType::Local;
    const std::string threadId = tm.createThread(metadata);

    EditBatchSummary summary;
    summary.editBatchId = "edit-batch";
    summary.threadId = threadId;
    summary.status = EditBatchStatus::Undone;
    summary.toolName = "EditWrite";
    summary.summaryText = "edited";

    EditFileMutation mutation;
    mutation.fileMutationId = "mutation-1";
    mutation.editBatchId = summary.editBatchId;
    mutation.threadId = threadId;
    mutation.filePath = "a.txt";
    mutation.status = EditFileMutationStatus::Undone;

    tm.writeEditBatch(threadId, summary, {mutation});

    auto eligibility = Engine::instance().evaluateEditBatchUndo(threadId, summary.editBatchId);
    EXPECT_EQ(eligibility.resultStatus, EditUndoResultStatus::RejectedAlreadyUndone);
    EXPECT_FALSE(eligibility.undoable);
}

TEST_F(EditUndoSmokeTest, UndoPersistsActionSoRedoCanReplayBatch) {
    ThreadManager tm(tempDir + "/.firmius/threads");
    ThreadMetadata metadata;
    metadata.title = "undo";
    metadata.hostOptions.type = HostType::Local;
    const std::string threadId = tm.createThread(metadata);

    EditBatchSummary summary;
    summary.editBatchId = "edit-batch";
    summary.threadId = threadId;
    summary.agentId = "agent-1";
    summary.toolName = "EditWrite";
    summary.summaryText = "edited";
    summary.status = EditBatchStatus::Applied;
    summary.files = {"a.txt"};

    EditFileMutation mutation;
    mutation.fileMutationId = "mutation-1";
    mutation.editBatchId = summary.editBatchId;
    mutation.threadId = threadId;
    mutation.filePath = "a.txt";
    mutation.hadFileBefore = true;
    mutation.hasFileAfter = true;
    mutation.status = EditFileMutationStatus::Applied;
    mutation.operations.push_back(
        EditMutationOperation{"replace line", 1, 1, {"old line"}, {"new line"}});

    tm.writeEditBatch(threadId, summary, {mutation});

    AgentContext context;
    context.history = std::make_shared<AgentHistory>();
    context.history->threadId = threadId;
    context.identity.id = "agent-1";

    auto env = std::make_shared<firmius::test::MockEnvironment>();
    auto host = std::dynamic_pointer_cast<firmius::test::MockHost>(env->getHost());
    ASSERT_TRUE(host);
    const std::string absolutePath = tempDir + "/workspace/a.txt";
    host->writeFile(absolutePath,
                    std::vector<uint8_t>{'n', 'e', 'w', ' ', 'l', 'i', 'n', 'e'});

    EXPECT_CALL(env->mockWorkspace(), resolvePath("a.txt"))
        .WillRepeatedly(testing::Return(absolutePath));
    EXPECT_CALL(env->mockWorkspace(), recordFileEdit(absolutePath))
        .Times(2);

    auto agent = std::make_shared<firmius::test::MockAgent>(context, env);
    AgentRegistry::instance().registerAgent("agent-1", agent);

    const auto undoAction = Engine::instance().undoEditBatch("agent-1", summary.editBatchId);
    EXPECT_EQ(undoAction.resultStatus, EditUndoResultStatus::Succeeded);

    const auto persistedUndo = tm.findEditUndoAction(threadId, undoAction.undoActionId);
    ASSERT_TRUE(persistedUndo.has_value());
    EXPECT_EQ(persistedUndo->targetEditBatchId, summary.editBatchId);

    const auto redoEligibility =
        Engine::instance().evaluateEditBatchRedo(threadId, undoAction.undoActionId);
    EXPECT_TRUE(redoEligibility.redoable);

    const auto redoAction =
        Engine::instance().redoEditUndoAction("agent-1", undoAction.undoActionId);
    ASSERT_TRUE(redoAction.has_value());
    EXPECT_EQ(tm.getEditBatch(threadId, summary.editBatchId).summary.status,
              EditBatchStatus::Redone);
}

} // namespace
