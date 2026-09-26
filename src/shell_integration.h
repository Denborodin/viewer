#pragma once
#include "common.h"
namespace viewer {
struct ShellResult {
    bool success;
    std::wstring text;
};
ShellResult shellIntegration(std::wstring_view action);
} // namespace viewer
