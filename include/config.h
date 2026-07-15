#pragma once
#include <string>
#include "json.hpp"

class Config {
public:
    static Config* instance();

    bool load(const std::string& path);

    std::string getString(const std::string& section,
                          const std::string& key,
                          const std::string& defaultValue = "") const;
    int         getInt(const std::string& section,
                       const std::string& key,
                       int defaultValue = 0) const;

private:
    Config() = default;
    nlohmann::json root_;
};