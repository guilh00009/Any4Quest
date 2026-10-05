#pragma once
#include <filesystem>
namespace Common::FS {
enum class PathType { UserDir };
inline std::filesystem::path GetUserPath(PathType) {
    return std::filesystem::temp_directory_path() / "any4quest-focused-test-no-config";
}
}
