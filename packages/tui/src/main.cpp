#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "AppState.hpp"
#include "ChatRenderer.hpp"
#include "CommandHelper.hpp"
#include "CommandRegistry.hpp"
#include "EventBridge.hpp"
#include "FooterBar.hpp"
#include "InputComponent.hpp"
#include "ModalSystem.hpp"
#include "SubagentStrip.hpp"
#include "WelcomeScreen.hpp"
#include "harness/Harness.hpp"
#include "Enums.hpp"

namespace {

std::atomic<bool> g_exitRequested{false};
ftxui::ScreenInteractive* g_screen = nullptr;

constexpr auto ANIMATION_FRAME_INTERVAL = std::chrono::milliseconds(33);

void handleSignal(int) {
    g_exitRequested.store(true, std::memory_order_relaxed);
    if (g_screen) {
        g_screen->Exit();
    }
}

struct CliArgs {
    std::string cwd;
    std::string threadId;
};

CliArgs parseCliArgs(int argc, char** argv) {
    CliArgs args;
    args.cwd = std::filesystem::current_path().string();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--cwd" && i + 1 < argc) {
            args.cwd = argv[++i];
        } else if (arg == "--thread" && i + 1 < argc) {
            args.threadId = argv[++i];
        }
    }

    return args;
}

}

#include <iostream>
#include <stdexcept>
#include <sstream>
#include "Panic.hpp"

namespace {
constexpr auto PANIC_INFO_TUI_STATE = "tui_state";
}

int main(int argc, char** argv) {
    firmius::shared::Panic::init();
    try {
        auto cliArgs = parseCliArgs(argc, argv);

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        auto& harness = firmius::core::Harness::instance();
        harness.init();

        auto appState = std::make_shared<firmius::tui::AppState>();
        auto screen = ftxui::ScreenInteractive::Fullscreen();
        g_screen = &screen;

        firmius::shared::Panic::addExtraInfo(PANIC_INFO_TUI_STATE, [&appState]() -> std::string {
            std::stringstream ss;
            ss << "Focused Agent: " << (appState->getFocusedAgentId().empty() ? "<none>" : appState->getFocusedAgentId()) << "\n";
            ss << "Message Count: " << appState->getMessages().size() << "\n";
            ss << "Streaming: " << (appState->getStreamingMessage().has_value() ? "yes" : "no") << "\n";
            ss << "Exit Requested: " << (g_exitRequested.load() ? "yes" : "no") << "\n";
            return ss.str();
        });

        firmius::shared::Panic::setPrePanicCallback([&]() {
            if (g_screen) {
                g_screen->Exit();
                std::cerr << "\n[TUI] Screen exited due to panic." << std::endl;
            }
        });

        firmius::tui::EventBridge eventBridge(*appState, screen);
        int subscriptionId = harness.subscribe(
            [&eventBridge](const firmius::harness::HarnessEvent& event) {
                eventBridge.push(event);
            });

        if (!cliArgs.threadId.empty()) {
            harness.switchThread(cliArgs.threadId);
        } else {
            harness.resumeLast();
        }

        firmius::tui::ChatRenderer chatRenderer;
        firmius::tui::CommandRegistry commandRegistry;
        firmius::tui::ModalSystem modalSystem(appState);
        commandRegistry.init(modalSystem);
        firmius::tui::CommandHelper commandHelper(commandRegistry, *appState);
        firmius::tui::FooterBar footerBar(appState);
        firmius::tui::WelcomeScreen welcomeScreen(appState);
        auto subagentStrip = ftxui::Make<firmius::tui::SubagentStrip>(appState);

        auto inputComponent = firmius::tui::MakeInputComponent({
            .onSubmit = [&](std::string text) {
                if (text.empty()) return;
                if (commandRegistry.execute(text)) return;

                if (harness.currentThreadId().empty()) {
                    harness.newThread(firmius::shared::HostType::Local, cliArgs.cwd);
                }
                harness.send(text);
            },
            .onInterrupt = [&]() {
                harness.abort();
            },
            .isVisionSupported = []() -> bool {
                return false;
            },
            .renderHelper = [&commandHelper](std::string input) -> ftxui::Element {
                return commandHelper.render(input);
            },
        });

        auto container = ftxui::Container::Vertical({
            subagentStrip,
            inputComponent,
        });
        container->SetActiveChild(inputComponent);

        auto mainComponent = ftxui::Renderer(container, [&]() {
            auto messages = appState->getMessages();
            auto streaming = appState->getStreamingMessage();

            auto allMessages = messages;
            if (streaming.has_value()) {
                allMessages.push_back(streaming.value());
            }

            auto dims = ftxui::Terminal::Size();
            int chatHeight = std::max(1, dims.dimy - 4);

            auto chatElement = allMessages.empty()
                ? welcomeScreen.Render()
                : chatRenderer.render(
                      allMessages, dims.dimx, chatHeight,
                      appState->getFocusedAgentId());

            auto mainContent = ftxui::vbox({
                chatElement | ftxui::flex,
                container->Render(),
                footerBar.Render(),
            });

            return modalSystem.Render(mainContent);
        });

        mainComponent = ftxui::CatchEvent(mainComponent, [&](ftxui::Event event) -> bool {
            if (event == ftxui::Event::Custom) {
                eventBridge.drain();
                return false;
            }

            if (modalSystem.isActive()) {
                return modalSystem.HandleEvent(event);
            }

            return container->OnEvent(event);
        });

        std::atomic<bool> animationRunning{true};
        std::thread animationThread([&]() {
            while (animationRunning.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(ANIMATION_FRAME_INTERVAL);
                if (g_exitRequested.load(std::memory_order_relaxed)) {
                    screen.Exit();
                    break;
                }
                screen.PostEvent(ftxui::Event::Custom);
            }
        });

        screen.Loop(mainComponent);

        animationRunning.store(false, std::memory_order_relaxed);
        if (animationThread.joinable()) {
            animationThread.join();
        }

        if (g_exitRequested.load()) {
            harness.abort();
            harness.writeInterruptionRecord();
        }

        harness.unsubscribe(subscriptionId);
        harness.shutdown();
        firmius::shared::Panic::removeExtraInfo(PANIC_INFO_TUI_STATE);
        g_screen = nullptr;

        return 0;
    } catch (const std::exception& e) {
        if (g_screen) {
            g_screen->Exit();
        }
        firmius::shared::Panic::removeExtraInfo(PANIC_INFO_TUI_STATE);
        std::cerr << "\n[FATAL ERROR] Firmius TUI crashed!" << std::endl;
        std::cerr << "Reason: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        if (g_screen) {
            g_screen->Exit();
        }
        firmius::shared::Panic::removeExtraInfo(PANIC_INFO_TUI_STATE);
        std::cerr << "\n[FATAL ERROR] Firmius TUI crashed with unknown exception!" << std::endl;
        return 1;
    }
}
