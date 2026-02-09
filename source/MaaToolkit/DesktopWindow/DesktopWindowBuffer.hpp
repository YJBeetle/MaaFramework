#pragma once

#include <ostream>

#include "API/MaaToolkitBufferTypes.hpp"
#include "Common/Conf.h"
#include "MaaUtils/Buffer/ListBuffer.hpp"
#include "MaaUtils/Encoding.h"
#include "MaaUtils/Logger.h"

MAA_TOOLKIT_NS_BEGIN

struct DesktopWindow
{
    // Win32 字段
    void* hwnd = nullptr;
    std::wstring class_name;
    std::wstring window_name;

    // MacOS 字段
    uint32_t window_id = 0;      // macOS 窗口ID
    std::string title;           // macOS 窗口标题
    int32_t pid = 0;             // macOS 进程ID
    std::string bundleId;        // macOS Bundle ID
    std::string applicationName; // macOS 应用程序名称

    MEO_TOJSON(hwnd, class_name, window_name, window_id, title, pid, bundleId, applicationName);
};

class DesktopWindowBuffer : public MaaToolkitDesktopWindow
{
public:
    DesktopWindowBuffer(const DesktopWindow& window)
        : hwnd_(window.hwnd)
        , class_name_(from_u16(window.class_name))
        , window_name_(from_u16(window.window_name))
        , window_id_(window.window_id)
        , title_(window.title)
        , pid_(window.pid)
        , bundleId_(window.bundleId)
        , applicationName_(window.applicationName)
    {
    }

    virtual ~DesktopWindowBuffer() override = default;

    virtual void* handle() const override { return hwnd_; }

    virtual const std::string& class_name() const override { return class_name_; }

    virtual const std::string& window_name() const override { return window_name_; }

    virtual uint32_t window_id() const override { return window_id_; }

    virtual const std::string& title() const override { return title_; }

    virtual int32_t pid() const override { return pid_; }

    virtual const std::string& bundle_id() const override { return bundleId_; }

    virtual const std::string& application_name() const override { return applicationName_; }

private:
    void* hwnd_ = nullptr;
    std::string class_name_;
    std::string window_name_;
    uint32_t window_id_ = 0;
    std::string title_;
    int32_t pid_ = 0;
    std::string bundleId_;
    std::string applicationName_;
};

MAA_TOOLKIT_NS_END

struct MaaToolkitDesktopWindowList : public MAA_NS::ListBuffer<MAA_TOOLKIT_NS::DesktopWindowBuffer>
{
    virtual ~MaaToolkitDesktopWindowList() override = default;
};
