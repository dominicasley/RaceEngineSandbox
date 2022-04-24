#include "DotNetHost.h"

#include <Windows.h>
#include <spdlog/spdlog.h>
#include <coreclr/hosts/inc/coreclrhost.h>
#include <filesystem>

#define FS_SEPARATOR "\\"
#define PATH_DELIMITER ";"

class Test {
public:
    int width;
    int height;
private:
    int count;
};

typedef Test* (*createTest)(int width, int height);
typedef void (*updateTest)(Test** test);

DotNetHost::DotNetHost() {
    const auto currentDir = std::filesystem::current_path();
    const auto coreClrDir = std::filesystem::path(R"(C:\Users\Dominic\Documents\dev\OpenStreetRace\projects\NetHostTest\HelloWorld\bin)");
    const auto coreClrPath = coreClrDir.string().append("\\coreclr.dll");

    HMODULE coreClr = LoadLibraryExA(
        coreClrPath.c_str(),
        nullptr,
        0);


    if (coreClr == nullptr) {
        spdlog::error("Could not load core clr at path {}", coreClrPath);
        return;
    } else {
        spdlog::info("Loaded core clr");
    }

    auto initializeCoreClr = reinterpret_cast<coreclr_initialize_ptr>(GetProcAddress(coreClr, "coreclr_initialize"));
    auto createManagedDelegate = reinterpret_cast<coreclr_create_delegate_ptr>(GetProcAddress(coreClr, "coreclr_create_delegate"));
    auto shutdownCoreClr = reinterpret_cast<coreclr_shutdown_ptr>(GetProcAddress(coreClr, "coreclr_shutdown"));

    if (initializeCoreClr == nullptr || createManagedDelegate == nullptr || shutdownCoreClr == nullptr) {
        spdlog::error("Required core clr functions not found");
        return;
    }

    std::string tpaList;
    buildTpaList(coreClrDir, ".dll", tpaList);

    const char* propertyKeys[] = {
        "APP_PATHS",
        "TRUSTED_PLATFORM_ASSEMBLIES"
    };

    const auto appPath = coreClrDir.string();
    const char* propertyValues[] = {
        appPath.c_str(),
        tpaList.c_str()
    };
    void* hostHandle;
    unsigned int domainId;

    int coreClrInitHostResult = initializeCoreClr(
        appPath.c_str(),
        "host",
        sizeof(propertyKeys) / sizeof(char*),
        propertyKeys, propertyValues, &hostHandle, &domainId);

    if (coreClrInitHostResult >= 0) {
        spdlog::info("core clr runtime started");
    } else {
        spdlog::error("Failed to initialise core clr host {0:x}", coreClrInitHostResult);
    }

    createTest createTestFnPtr;
    auto createManagedDelegateResult = createManagedDelegate(
        hostHandle,
        domainId,
        "HelloWorld",
        "Test",
        "Create",
        reinterpret_cast<void **>(&createTestFnPtr));

    if (createManagedDelegateResult >= 0)
    {
        spdlog::info("Managed delegate created");
    }
    else
    {
        spdlog::error("coreclr_create_delegate failed: {0:x}", createManagedDelegateResult);
        return;
    }

    updateTest updateTestFnPtr;

    createManagedDelegate(
        hostHandle,
        domainId,
        "HelloWorld",
        "Test",
        "_update",
        reinterpret_cast<void **>(&updateTestFnPtr));

    auto test = createTestFnPtr(10, 15);

    test->width = 100;
    updateTestFnPtr(&test);

    shutdownCoreClr(hostHandle, domainId);
}

void DotNetHost::buildTpaList(const std::filesystem::path& directory, const char* extension, std::string& tpaList) const
{
    std::string searchPath = directory.string();
    searchPath.append(FS_SEPARATOR);
    searchPath.append("*");
    searchPath.append(extension);

    WIN32_FIND_DATAA findData;
    HANDLE fileHandle = FindFirstFileA(searchPath.c_str(), &findData);

    if (fileHandle != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::string file = directory.string();
            file.append(directory.string());
            file.append(FS_SEPARATOR);
            file.append(findData.cFileName);
            file.append(PATH_DELIMITER);

            spdlog::info("Adding trusted platform assembly: {}", findData.cFileName);
            tpaList.append(file);
        }
        while (FindNextFileA(fileHandle, &findData));
        FindClose(fileHandle);
    }
}