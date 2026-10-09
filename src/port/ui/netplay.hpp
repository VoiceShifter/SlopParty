#pragma once

#include "window.hpp"

#include <string>

namespace partyboard::ui {

class NetplayWindow final : public Window {
public:
    NetplayWindow();

private:
    std::string mAddress = "127.0.0.1";
    int mPort = 46040;
    int mInputDelay = 4;
};

} // namespace partyboard::ui
