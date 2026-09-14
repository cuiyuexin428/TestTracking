#pragma once
#include <string>
#include <vector>
namespace stpcompare {
// Parse an old command template into literal argv before replacing placeholders.
// Supports quotes and Windows backslash-before-quote rules; invokes no shell.
bool ParseProgramArguments(const std::string& command,std::vector<std::string>* arguments);
}
