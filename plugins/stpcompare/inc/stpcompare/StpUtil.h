#pragma once
#include <string>
namespace stpcompare {
std::string HexEncode(const std::string& bytes);
bool HexDecode(const std::string& hex,std::string* bytes);
}
