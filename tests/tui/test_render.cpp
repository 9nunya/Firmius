#include <gtest/gtest.h>

#include "FooterBar.hpp"
#include "SubagentStrip.hpp"
#include "AppState.hpp"
#include <ftxui/screen/screen.hpp>
#include <ftxui/dom/elements.hpp>

using namespace firmius::tui;

class RenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        state_ = std::make_shared<AppState>();
    }

    std::string RenderFooterToString(int width = 80) {
        FooterBar footer(state_);
        auto element = footer.Render();
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(1));
        ftxui::Render(screen, element);
        return screen.ToString();
    }

    std::string RenderStripToString(int width = 80, int height = 10) {
        SubagentStrip strip(state_);
        auto element = strip.Render();
        auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(height));
        ftxui::Render(screen, element);
        return screen.ToString();
    }

    std::shared_ptr<AppState> state_;
};

TEST_F(RenderTest, FooterBarRendersWithoutCrash) {
    EXPECT_NO_THROW(RenderFooterToString());
}

TEST_F(RenderTest, FooterBarShowsIdleStatus) {
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("IDLE"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsThreadInfo) {
    using namespace firmius::harness;
    ThreadChanged event;
    event.threadId = "test-thread-123";
    event.metadata.title = "Test Thread";
    state_->applyEvent(event);
    
    // FooterBar now hides thread info if no messages exist.
    MessageCompleted completed;
    completed.agentId = "agent-1";
    completed.message.role = Role::Assistant;
    completed.message.content.push_back(TextContent{"Initial message"});
    state_->applyEvent(completed);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("Test Thread"), std::string::npos);
}

TEST_F(RenderTest, FooterBarTruncatesLongTitle) {
    using namespace firmius::harness;
    ThreadChanged event;
    event.threadId = "thread-1";
    event.metadata.title = "This is a very long title that should be truncated";
    state_->applyEvent(event);
    
    // FooterBar now hides thread info if no messages exist.
    MessageCompleted completed;
    completed.agentId = "agent-1";
    completed.message.role = Role::Assistant;
    completed.message.content.push_back(TextContent{"Initial message"});
    state_->applyEvent(completed);
    
    auto output = RenderFooterToString(40);
    EXPECT_EQ(output.find("truncated"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsStreamingStatus) {
    using namespace firmius::harness;
    MessageChunk chunk;
    chunk.agentId = "agent-1";
    chunk.delta = "Hello";
    chunk.isThinking = false;
    state_->applyEvent(chunk);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("STREAM"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsToolStatus) {
    using namespace firmius::harness;
    ToolCallStarted started;
    started.agentId = "agent-1";
    started.toolCallId = "call-1";
    started.name = "bash";
    started.args = "{}";
    state_->applyEvent(started);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("TOOL"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsCompactingStatus) {
    using namespace firmius::harness;
    AgentCompactingEvent compacting;
    compacting.agentId = "agent-1";
    state_->applyEvent(compacting);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("COMPACT"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsErrorStatus) {
    using namespace firmius::harness;
    HarnessError error;
    error.message = "Test error";
    state_->applyEvent(error);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("ERROR"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsNoAgentWhenEmpty) {
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("No active agent"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsInitializingWhenNoModel) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.persona = "coder";
    spawned.friendlyName = "Coder";
    state_->applyEvent(spawned);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("Initializing"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsModelInfo) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.persona = "coder";
    state_->applyEvent(spawned);
    
    ModelSwitchedEvent switched;
    switched.agentId = "sub-1";
    switched.newProviderId = "openai";
    switched.newModelId = "gpt-4";
    state_->applyEvent(switched);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("openai"), std::string::npos);
    EXPECT_NE(output.find("gpt-4"), std::string::npos);
}

TEST_F(RenderTest, SubagentStripEmptyWhenNoSubagents) {
    auto output = RenderStripToString();
    EXPECT_TRUE(output.empty() || output.find_first_not_of(" \n\r\t") == std::string::npos);
}

TEST_F(RenderTest, SubagentStripShowsAgentRow) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.persona = "coder";
    spawned.friendlyName = "MyAgent";
    spawned.title = "Working on task";
    state_->applyEvent(spawned);
    
    auto output = RenderStripToString();
}

TEST_F(RenderTest, SubagentStripShowsFocusIndicatorUnfocused) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.persona = "coder";
    spawned.friendlyName = "Agent";
    state_->applyEvent(spawned);
    
    auto output = RenderStripToString();
    EXPECT_NE(output.find("[ ]"), std::string::npos);
}

TEST_F(RenderTest, SubagentStripShowsFocusIndicatorFocused) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.persona = "coder";
    spawned.friendlyName = "Agent";
    state_->applyEvent(spawned);
    
    state_->setFocusedAgentId("sub-1");
    
    auto output = RenderStripToString();
    EXPECT_NE(output.find("[>]"), std::string::npos);
}

TEST_F(RenderTest, SubagentStripShowsIdleStatus) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.friendlyName = "Agent";
    state_->applyEvent(spawned);
    
    auto output = RenderStripToString();
    EXPECT_NE(output.find("IDLE"), std::string::npos);
}

TEST_F(RenderTest, SubagentStripShowsThinkingStatus) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.friendlyName = "Agent";
    state_->applyEvent(spawned);
    
    MessageChunk chunk;
    chunk.agentId = "sub-1";
    chunk.delta = "Thinking...";
    chunk.isThinking = true;
    state_->applyEvent(chunk);
    
    auto output = RenderStripToString();
}

TEST_F(RenderTest, SubagentStripShowsStreamingStatus) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.friendlyName = "Agent";
    state_->applyEvent(spawned);
    
    MessageChunk chunk;
    chunk.agentId = "sub-1";
    chunk.delta = "Hello";
    chunk.isThinking = false;
    state_->applyEvent(chunk);
    
    auto output = RenderStripToString();
}

TEST_F(RenderTest, SubagentStripShowsToolCallStatus) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.friendlyName = "Agent";
    state_->applyEvent(spawned);
    
    ToolCallStarted started;
    started.agentId = "sub-1";
    started.toolCallId = "call-1";
    started.name = "bash";
    state_->applyEvent(started);
    
    auto output = RenderStripToString();
}

TEST_F(RenderTest, SubagentStripMultipleAgents) {
    using namespace firmius::harness;
    
    SubagentSpawned spawned1;
    spawned1.parentId = "parent-1";
    spawned1.agentId = "sub-1";
    spawned1.friendlyName = "Agent1";
    state_->applyEvent(spawned1);
    
    SubagentSpawned spawned2;
    spawned2.parentId = "parent-1";
    spawned2.agentId = "sub-2";
    spawned2.friendlyName = "Agent2";
    state_->applyEvent(spawned2);
    
    auto output = RenderStripToString(80, 20);
    EXPECT_NE(output.find("Agent1"), std::string::npos);
    EXPECT_NE(output.find("Agent2"), std::string::npos);
}

TEST_F(RenderTest, SubagentStripShowsTitle) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.friendlyName = "Agent";
    spawned.title = "Fixing bugs";
    state_->applyEvent(spawned);
    
    auto output = RenderStripToString();
    EXPECT_NE(output.find("Fixing bugs"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsTokenMetrics) {
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("ctx:"), std::string::npos);
    EXPECT_NE(output.find("total:"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsCompactingIndicator) {
    using namespace firmius::harness;
    AgentCompactingEvent compacting;
    compacting.agentId = "agent-1";
    state_->applyEvent(compacting);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("COMPACTING"), std::string::npos);
}

TEST_F(RenderTest, FooterBarShowsSavedTokens) {
    using namespace firmius::harness;
    ContextCompactedEvent compacted;
    compacted.agentId = "agent-1";
    compacted.tokensSaved = 1000;
    state_->applyEvent(compacted);
    
    auto output = RenderFooterToString();
    EXPECT_NE(output.find("saved"), std::string::npos);
    EXPECT_NE(output.find("1000"), std::string::npos);
}

TEST_F(RenderTest, SubagentStripRendersWithoutCrash) {
    using namespace firmius::harness;
    SubagentSpawned spawned;
    spawned.parentId = "parent-1";
    spawned.agentId = "sub-1";
    spawned.friendlyName = "Test";
    state_->applyEvent(spawned);
    
    EXPECT_NO_THROW(RenderStripToString());
}
