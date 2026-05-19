#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace Zaslon {
namespace Abstraction {

// --------------------------------------------------------------------------
// 1. Interfaces (Abstract Products)
// --------------------------------------------------------------------------

// Interface for File System Operations
class IFileSystemManager {
public:
    virtual ~IFileSystemManager() = default;
    virtual bool FileExists(const std::wstring& path) const = 0;
    virtual std::vector<std::wstring> EnumerateDirectory(const std::wstring& path) const = 0;
    virtual std::optional<std::vector<uint8_t>> ReadFileRaw(const std::wstring& path) const = 0;
};

// Interface for Registry Operations
class IRegistryManager {
public:
    virtual ~IRegistryManager() = default;
    virtual std::optional<std::wstring> ReadStringValue(const std::wstring& key_path, const std::wstring& value_name) const = 0;
    virtual std::vector<std::wstring> EnumerateSubKeys(const std::wstring& key_path) const = 0;
};

// Interface for Process Operations
class IProcessManager {
public:
    virtual ~IProcessManager() = default;
    virtual std::vector<uint32_t> EnumerateProcesses() const = 0;
    virtual bool TerminateProcess(uint32_t pid) const = 0;
};

// --------------------------------------------------------------------------
// 2. Concrete Products: Live API (Standard Windows)
// --------------------------------------------------------------------------

class LiveFileSystemManager : public IFileSystemManager {
public:
    bool FileExists(const std::wstring& path) const override;
    std::vector<std::wstring> EnumerateDirectory(const std::wstring& path) const override;
    std::optional<std::vector<uint8_t>> ReadFileRaw(const std::wstring& path) const override;
};

class LiveRegistryManager : public IRegistryManager {
public:
    std::optional<std::wstring> ReadStringValue(const std::wstring& key_path, const std::wstring& value_name) const override;
    std::vector<std::wstring> EnumerateSubKeys(const std::wstring& key_path) const override;
};

class LiveProcessManager : public IProcessManager {
public:
    std::vector<uint32_t> EnumerateProcesses() const override;
    bool TerminateProcess(uint32_t pid) const override;
};

// --------------------------------------------------------------------------
// 3. Concrete Products: Offline WinPE (Raw Parsing / Offline Mounting)
// --------------------------------------------------------------------------

class OfflineFileSystemManager : public IFileSystemManager {
public:
    explicit OfflineFileSystemManager(std::wstring target_os_drive) : m_target_drive(std::move(target_os_drive)) {}
    bool FileExists(const std::wstring& path) const override;
    std::vector<std::wstring> EnumerateDirectory(const std::wstring& path) const override;
    std::optional<std::vector<uint8_t>> ReadFileRaw(const std::wstring& path) const override;
private:
    std::wstring m_target_drive;
    std::wstring TranslatePath(const std::wstring& path) const;
};

class OfflineRegistryManager : public IRegistryManager {
public:
    OfflineRegistryManager() = default;
    bool InitializeOfflineHives(const std::wstring& system_root);
    std::optional<std::wstring> ReadStringValue(const std::wstring& key_path, const std::wstring& value_name) const override;
    std::vector<std::wstring> EnumerateSubKeys(const std::wstring& key_path) const override;
private:
    // Mappings between virtual paths (e.g. HKLM\System) to offline mounted keys (e.g. HKLM\Zaslon_Offline_System)
    std::wstring TranslateKeyPath(const std::wstring& path) const;
};

class OfflineProcessManager : public IProcessManager {
public:
    std::vector<uint32_t> EnumerateProcesses() const override {
        // In WinPE offline mode, the target OS processes are not running.
        return {};
    }
    bool TerminateProcess(uint32_t pid) const override {
        return false;
    }
};

// --------------------------------------------------------------------------
// 4. Abstract Factory
// --------------------------------------------------------------------------

class IOSFactory {
public:
    virtual ~IOSFactory() = default;
    virtual std::unique_ptr<IFileSystemManager> CreateFileSystemManager() const = 0;
    virtual std::unique_ptr<IRegistryManager> CreateRegistryManager() const = 0;
    virtual std::unique_ptr<IProcessManager> CreateProcessManager() const = 0;
};

class LiveOSFactory : public IOSFactory {
public:
    std::unique_ptr<IFileSystemManager> CreateFileSystemManager() const override {
        return std::make_unique<LiveFileSystemManager>();
    }
    std::unique_ptr<IRegistryManager> CreateRegistryManager() const override {
        return std::make_unique<LiveRegistryManager>();
    }
    std::unique_ptr<IProcessManager> CreateProcessManager() const override {
        return std::make_unique<LiveProcessManager>();
    }
};

class OfflineOSFactory : public IOSFactory {
public:
    explicit OfflineOSFactory(std::wstring target_drive) : m_target_drive(std::move(target_drive)) {}

    std::unique_ptr<IFileSystemManager> CreateFileSystemManager() const override {
        return std::make_unique<OfflineFileSystemManager>(m_target_drive);
    }
    std::unique_ptr<IRegistryManager> CreateRegistryManager() const override {
        auto rm = std::make_unique<OfflineRegistryManager>();
        // Initialization would happen here or deferred
        rm->InitializeOfflineHives(m_target_drive + L"\\Windows");
        return rm;
    }
    std::unique_ptr<IProcessManager> CreateProcessManager() const override {
        return std::make_unique<OfflineProcessManager>();
    }
private:
    std::wstring m_target_drive;
};

// --------------------------------------------------------------------------
// 5. Environment Context (Singleton to provide global OS Factory)
// --------------------------------------------------------------------------

class EnvironmentContext {
public:
    static EnvironmentContext& GetInstance() {
        static EnvironmentContext instance;
        return instance;
    }

    void Initialize(bool is_winpe, const std::wstring& target_drive = L"") {
        if (is_winpe) {
            m_factory = std::make_unique<OfflineOSFactory>(target_drive);
        } else {
            m_factory = std::make_unique<LiveOSFactory>();
        }
    }

    IOSFactory* GetFactory() const {
        return m_factory.get();
    }

private:
    EnvironmentContext() = default;
    std::unique_ptr<IOSFactory> m_factory;
};

} // namespace Abstraction
} // namespace Zaslon
