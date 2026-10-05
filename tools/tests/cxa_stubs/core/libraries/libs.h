#pragma once
#include <set>
#include <string>
namespace Core::Loader {
class SymbolsResolver { public: std::set<std::string> exports; };
}
#define LIB_FUNCTION(nid, lib, version, mod, function) \
    sym->exports.insert(std::string(lib) + ":" + nid); (void)function;
