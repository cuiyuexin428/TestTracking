#include "stpcompare/StpUtil.h"
namespace stpcompare {
std::string HexEncode(const std::string& bytes) {
    static constexpr char digits[]="0123456789abcdef";
    std::string hex; hex.reserve(bytes.size()*2);
    for(const unsigned char value:bytes) {
        hex.push_back(digits[value>>4]);
        hex.push_back(digits[value&15]);
    }
    return hex;
}

bool HexDecode(const std::string& hex,std::string* bytes) {
    if(!bytes || hex.size()%2) return false;
    auto nibble=[](char value)->int {
        if(value>='0'&&value<='9') return value-'0';
        if(value>='a'&&value<='f') return value-'a'+10;
        if(value>='A'&&value<='F') return value-'A'+10;
        return -1;
    };
    bytes->clear(); bytes->reserve(hex.size()/2);
    for(std::size_t i=0;i<hex.size();i+=2) {
        const auto high=nibble(hex[i]),low=nibble(hex[i+1]);
        if(high<0||low<0) { bytes->clear(); return false; }
        bytes->push_back(static_cast<char>((high<<4)|low));
    }
    return true;
}
}
