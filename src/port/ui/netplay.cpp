#include "netplay.hpp"

#include "modal.hpp"
#include "number_button.hpp"
#include "pane.hpp"
#include "prelaunch.hpp"
#include "string_button.hpp"
#include "port/main.h"
#include "port/netplay.h"

#include <atomic>
#include <thread>
#include <utility>

namespace partyboard::ui {
namespace {

enum class ConnectionMode {
    Host,
    Join,
};

Rml::String connection_status(ConnectionMode mode, const std::string &address, int port)
{
    Rml::String status = "<span id=\"netplay-status\">";
    if (mode == ConnectionMode::Host) {
        status += "Waiting for a player on TCP port ";
        status += std::to_string(port);
        status += "...";
    }
    else {
        status += "Connecting to ";
        status += escape(address);
        status += ":";
        status += std::to_string(port);
        status += "...";
    }
    status += "<br/><br/>The game will start automatically after both players connect.</span>";
    return status;
}

class NetplayConnectionModal final : public Modal {
public:
    NetplayConnectionModal(ConnectionMode mode, std::string address, int port, int inputDelay)
        : Modal(Modal::Props {
              .title = mode == ConnectionMode::Host ? "Host Network Game" : "Join Network Game",
              .bodyRml = connection_status(mode, address, port),
              .actions =
                  {
                      ModalAction {
                          .label = "Cancel",
                          .onPressed = [](Modal &modal) {
                              static_cast<NetplayConnectionModal &>(modal).cancel_connection();
                          },
                      },
                  },
              .onDismiss = [](Modal &modal) {
                  static_cast<NetplayConnectionModal &>(modal).cancel_connection();
              },
          })
        , mMode(mode)
        , mAddress(std::move(address))
        , mPort(port)
        , mInputDelay(inputDelay)
        , mStatus(mDocument->GetElementById("netplay-status"))
    {
        mWorker = std::thread([this] {
            const BOOL connected = mMode == ConnectionMode::Host
                ? partyboard_netplay_host(static_cast<u16>(mPort), static_cast<u32>(mInputDelay))
                : partyboard_netplay_join(mAddress.c_str(), static_cast<u16>(mPort));
            mSucceeded.store(connected != FALSE);
            mDone.store(true, std::memory_order_release);
        });
    }

    ~NetplayConnectionModal() override
    {
        stop_connection();
    }

    void update() override
    {
        Modal::update();
        if (mCancelled || mHandled || !mDone.load(std::memory_order_acquire)) {
            return;
        }

        if (mWorker.joinable()) {
            mWorker.join();
        }
        mHandled = true;
        if (mSucceeded.load()) {
            mLaunchCommitted = true;
            bool closingPrelaunch = false;
            for (auto &document : get_document_stack()) {
                if (dynamic_cast<Prelaunch *>(document.get()) != nullptr) {
                    closingPrelaunch = true;
                }
                if (closingPrelaunch) {
                    document->Document::hide(true);
                }
            }
            PartyBoard_IsGameLaunched = true;
            return;
        }

        const char *error = partyboard_netplay_last_error();
        if (mStatus != nullptr) {
            Rml::String message = "<strong>Connection failed.</strong><br/><br/>";
            message += escape(error != nullptr && error[0] != '\0' ? error : "Unknown network error.");
            message += "<br/><br/>Select Cancel to return.";
            mStatus->SetInnerRML(message);
        }
    }

private:
    bool handle_nav_command(Rml::Event &event, NavCommand cmd) override
    {
        if (cmd == NavCommand::Menu) {
            return true;
        }
        return Modal::handle_nav_command(event, cmd);
    }

    void cancel_connection()
    {
        if (mCancelled) {
            return;
        }
        mCancelled = true;
        stop_connection();
        pop();
    }

    void stop_connection()
    {
        if (mWorker.joinable()) {
            if (!mDone.load(std::memory_order_acquire)) {
                partyboard_netplay_cancel_pending();
            }
            mWorker.join();
        }
        if (!mLaunchCommitted && partyboard_netplay_active()) {
            partyboard_netplay_shutdown();
        }
    }

    ConnectionMode mMode;
    std::string mAddress;
    int mPort;
    int mInputDelay;
    Rml::Element *mStatus;
    std::thread mWorker;
    std::atomic_bool mDone = false;
    std::atomic_bool mSucceeded = false;
    bool mCancelled = false;
    bool mHandled = false;
    bool mLaunchCommitted = false;
};

} // namespace

NetplayWindow::NetplayWindow()
{
    mSuppressNavFallback = true;
    add_tab("Host", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Connection");
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props {
                .key = "TCP Port",
                .getValue = [this] { return mPort; },
                .setValue = [this](int value) { mPort = value; },
                .min = 1,
                .max = 65535,
            }),
            rightPane, [](Pane &pane) {
                pane.add_text("The client must use the same TCP port. Internet hosts may need to allow and forward this port.");
            });
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props {
                .key = "Input Delay",
                .getValue = [this] { return mInputDelay; },
                .setValue = [this](int value) { mInputDelay = value; },
                .min = 0,
                .max = 30,
                .suffix = " frames",
            }),
            rightPane, [](Pane &pane) {
                pane.add_text("Adds fixed input delay for both players. Increase this if the connection is unstable.");
            });

        leftPane.add_section("Start");
        leftPane.register_control(
            leftPane.add_button("Host Game").on_pressed([this] {
                push(std::make_unique<NetplayConnectionModal>(
                    ConnectionMode::Host, std::string {}, mPort, mInputDelay));
            }),
            rightPane, [](Pane &pane) {
                pane.add_rml("Wait for one client, then launch the game for both players.<br/><br/>The host controls player 1.");
            });
    });

    add_tab("Join", [this](Rml::Element *content) {
        auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
        auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

        leftPane.add_section("Connection");
        leftPane.register_control(
            leftPane.add_child<StringButton>(StringButton::Props {
                .key = "Host Address",
                .getValue = [this] { return mAddress; },
                .setValue = [this](Rml::String value) { mAddress = std::move(value); },
                .maxLength = 255,
            }),
            rightPane, [](Pane &pane) {
                pane.add_text("Enter the host's IPv4 address. Use 127.0.0.1 when testing two instances on one computer.");
            });
        leftPane.register_control(
            leftPane.add_child<NumberButton>(NumberButton::Props {
                .key = "TCP Port",
                .getValue = [this] { return mPort; },
                .setValue = [this](int value) { mPort = value; },
                .min = 1,
                .max = 65535,
            }),
            rightPane, [](Pane &pane) {
                pane.add_text("Use the TCP port selected by the host.");
            });

        leftPane.add_section("Start");
        leftPane.register_control(
            leftPane.add_button("Join Game").on_pressed([this] {
                push(std::make_unique<NetplayConnectionModal>(
                    ConnectionMode::Join, mAddress, mPort, mInputDelay));
            }),
            rightPane, [](Pane &pane) {
                pane.add_rml("Connect to the host, then launch the game for both players.<br/><br/>The client controls player 2.");
            });
    });
}

} // namespace partyboard::ui
