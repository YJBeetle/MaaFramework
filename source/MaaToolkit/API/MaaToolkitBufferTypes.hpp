#pragma once

#include <filesystem>
#include <string>

#include <MaaToolkit/MaaToolkitDef.h>

struct MaaToolkitAdbDevice
{
public:
    virtual ~MaaToolkitAdbDevice() = default;

    virtual const std::string& name() const = 0;
    virtual const std::string& adb_path() const = 0;
    virtual const std::string& address() const = 0;
    virtual MaaAdbScreencapMethod screencap_methods() const = 0;
    virtual MaaAdbInputMethod input_methods() const = 0;
    virtual const std::string& config() const = 0;
};

struct MaaToolkitDesktopWindow
{
public:
    virtual ~MaaToolkitDesktopWindow() = default;
    // Win32 字段
    virtual void* handle() const = 0;
    virtual const std::string& class_name() const = 0;
    virtual const std::string& window_name() const = 0;
    // MacOS 字段
    virtual uint32_t window_id() const = 0;                  // macOS 窗口ID
    virtual const std::string& title() const = 0;            // macOS 窗口标题
    virtual int32_t pid() const = 0;                         // macOS 进程ID
    virtual const std::string& bundle_id() const = 0;        // macOS Bundle ID
    virtual const std::string& application_name() const = 0; // macOS 应用程序名称
};
