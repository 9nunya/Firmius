#ifndef FIRMIUS_TUI_STATE_HPP
#define FIRMIUS_TUI_STATE_HPP

#include "Context.hpp"
#include "HarnessEvents.hpp"
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace firmius::core {
class Harness;
}

namespace firmius::tui {

struct ToolCallView;
struct TitleBarModel;
struct StatusBarModel;
struct InputBarModel;

class TuiState {
public:
    static TuiState& instance();

    TuiState(const TuiState&) = delete;
    TuiState& operator=(const TuiState&) = delete;

    TuiState(TuiState&&) = delete;
    TuiState& operator=(TuiState&&) = delete;

    void init(firmius::core::Harness& harness,
              const shared::ThreadMetadata& thread,
              const std::string& focused_agent_id);
    void attachScreen(ftxui::ScreenInteractive* screen);
    ftxui::Component root();
    void shutdown();

private:
    TuiState();
    ~TuiState() = default;

    struct StreamState {
        std::string thinking;
        std::string text;
        std::string compaction_thinking;
        std::string compaction_text;
        bool provider_waiting = false;
    };

    void onEvent(const harness::HarnessEvent& ev);
    std::string statusText() const;

    firmius::core::Harness* harness_ = nullptr;
    shared::ThreadMetadata thread_;
    std::string focused_agent_id_;
    std::shared_ptr<shared::AgentHistory> history_;
    int subscription_id_ = -1;
    ftxui::ScreenInteractive* screen_ = nullptr;

    std::mutex mu_;
    std::unordered_map<std::string, StreamState> streams_;
    std::unordered_map<std::string, std::shared_ptr<ToolCallView>> tool_calls_;
    std::vector<std::string> tool_order_;

    std::string input_;
    int cursor_ = 0;

    std::shared_ptr<TitleBarModel> title_model_;
    std::shared_ptr<StatusBarModel> status_model_;
    std::shared_ptr<InputBarModel> input_model_;

    ftxui::Component root_component_;
    ftxui::Component chat_component_;
};

}

#endif
