#pragma once
#include <map>
#include <string>
#include <string_view>
namespace sinan {
std::string resolved_language(std::string preference);
class Messages {
    std::map<std::string,std::string,std::less<>> labels_;
public:
    explicit Messages(const std::string& preference="auto") {set_language(preference);}
    void set_language(const std::string& preference);
    std::string& operator[](std::string_view key);
};
}
