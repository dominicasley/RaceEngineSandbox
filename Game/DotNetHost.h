#pragma once

#include <filesystem>
#include <string>

class DotNetHost
{
private:
    void buildTpaList(const std::filesystem::path& directory, const char* extension, std::string& tpaList) const;

public:
    explicit DotNetHost();
};


