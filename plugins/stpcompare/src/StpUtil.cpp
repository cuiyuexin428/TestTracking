#include "stpcompare/StpUtil.h"
namespace stpcompare {
bool ParseProgramArguments(const std::string& command,std::vector<std::string>* arguments) {
    if(!arguments) return false;
    arguments->clear();
    std::size_t i=0;
    while(i<command.size()) {
        while(i<command.size() && (command[i]==' ' || command[i]=='\t')) ++i;
        if(i==command.size()) break;
        std::string argument; bool quoted=false;
        while(i<command.size() && (quoted || (command[i]!=' ' && command[i]!='\t'))) {
            std::size_t slashes=0;
            while(i<command.size() && command[i]=='\\') { ++slashes; ++i; }
            if(i<command.size() && command[i]=='"') {
                argument.append(slashes/2,'\\');
                if(slashes%2) argument+='"'; else quoted=!quoted;
                ++i;
            } else {
                argument.append(slashes,'\\');
                if(i<command.size()) argument+=command[i++];
            }
        }
        if(quoted) return false;
        arguments->push_back(argument);
    }
    return !arguments->empty();
}
}
