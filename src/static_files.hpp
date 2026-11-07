// Serves files from a document root, refusing anything outside it.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "http.hpp"

class StaticFiles {
public:
    explicit StaticFiles(const std::filesystem::path& root);

    http::Response serve(const http::Request& req) const;

    // Maps a decoded URL path to a regular file inside the root, or nullopt.
    // Rejects ".." segments, and also anything whose real path (after
    // following symlinks) lands outside the root.
    std::optional<std::filesystem::path> resolve(const std::string& decoded_path) const;

    static std::string mime_type(const std::filesystem::path& file);

    const std::filesystem::path& root() const { return root_; }

private:
    std::filesystem::path root_;  // canonical
};
