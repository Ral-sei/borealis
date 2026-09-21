#include <borealis/platforms/xbox360/xbox360_ime.hpp>
namespace brls {
bool Xbox360ImeManager::openForText(std::function<void(std::string)>, std::string, std::string, int, std::string, int) { return false; }
bool Xbox360ImeManager::openForNumber(std::function<void(long)>, std::string, std::string, int, std::string, std::string, std::string, int) { return false; }
}
