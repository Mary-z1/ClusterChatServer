#include "config.h"
#include <fstream>
#include <iostream>

Config* Config::instance() {
    static Config cfg;
    return &cfg;
}

bool Config::load(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "[Config] cannot open: " << path << std::endl;
        return false;
    }
    try {
        root_ = nlohmann::json::parse(ifs);
        std::cout << "[Config] loaded: " << path << std::endl;
        return true;
    } catch (const nlohmann::json::parse_error& e) {
        std::cerr << "[Config] parse error: " << e.what() << std::endl;
        return false;
    }
}

std::string Config::getString(const std::string& section,
                              const std::string& key,
                              const std::string& defaultValue) const {
    if (root_.contains(section) && root_[section].contains(key))
        return root_[section][key].get<std::string>();
    return defaultValue;
}

int Config::getInt(const std::string& section,
                   const std::string& key,
                   int defaultValue) const {
    if (root_.contains(section) && root_[section].contains(key))
        return root_[section][key].get<int>();
    return defaultValue;
}