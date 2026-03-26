#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>

#include "MaaFramework/MaaAPI.h"
#include "MaaToolkit/MaaToolkitAPI.h"

// 检查并请求必要权限
void checkAndRequestPermissions()
{
    bool has_screen_recording_permission = MaaToolkitMacOSCheckPermission(MaaMacOSPermissionScreenCapture);
    bool has_accessibility = MaaToolkitMacOSCheckPermission(MaaMacOSPermissionAccessibility);

    std::cout << "屏幕录制权限: " << (has_screen_recording_permission ? "已授权" : "未授权") << std::endl;
    std::cout << "辅助功能权限: " << (has_accessibility ? "已授权" : "未授权") << std::endl;

    // 屏幕录制权限
    while (!has_screen_recording_permission) {
        std::cout << "尝试请求屏幕录制权限..." << std::endl;
        // MaaToolkitMacOSRevealPermissionSettings(MaaMacOSPermissionScreenCapture);
        MaaToolkitMacOSRequestPermission(MaaMacOSPermissionScreenCapture);
        std::cout << "已发送请求，请在 系统设置 -> 隐私与安全性 -> 录屏与系统录音 中授予本应用权限。" << std::endl;

        std::cout << "是否重新检查？(Y/n): ";
        std::string choice;
        std::getline(std::cin, choice);
        if (choice == "n" || choice == "N") {
            std::cout << "请在授予权限后重启应用。" << std::endl;
            exit(0);
        }
        has_screen_recording_permission = MaaToolkitMacOSCheckPermission(MaaMacOSPermissionScreenCapture);
    }

    // 辅助功能权限
    while (!has_accessibility) {
        std::cout << "尝试请求辅助功能权限..." << std::endl;
        // MaaToolkitMacOSRevealPermissionSettings(MaaMacOSPermissionAccessibility);
        MaaToolkitMacOSRequestPermission(MaaMacOSPermissionAccessibility);
        std::cout << "已发送请求，请在 系统设置 -> 隐私与安全性 -> 辅助功能 中授予本应用权限。" << std::endl;

        std::cout << "是否重新检查？(Y/n): ";
        std::string choice2;
        std::getline(std::cin, choice2);
        if (choice2 == "n" || choice2 == "N") {
            std::cout << "请在授予权限后重启应用。" << std::endl;
            exit(0);
        }
        has_accessibility = MaaToolkitMacOSCheckPermission(MaaMacOSPermissionAccessibility);
    }
}

// 运行 MaaFW 窗口测试
void runMaaTest(const std::string& windowTitle, const std::string& exe_dir)
{
    // 初始化 MaaToolkit
    MaaToolkitConfigInitOption("./", "{}");

    uint32_t windowID = 0;

    // 使用 MaaFW 查找窗口
    auto winlist = MaaToolkitDesktopWindowListCreate();
    MaaToolkitDesktopWindowFindAll(winlist);

    size_t size = MaaToolkitDesktopWindowListSize(winlist);
    std::cout << "Found " << size << " windows." << std::endl;

    for (size_t i = 0; i < size; ++i) {
        auto w = MaaToolkitDesktopWindowListAt(winlist, i);
        std::string name = MaaToolkitDesktopWindowGetWindowName(w);
        uint32_t currentWindowID = reinterpret_cast<uintptr_t>(MaaToolkitDesktopWindowGetHandle(w));

        if (name.find(windowTitle) != std::string::npos) {
            windowID = currentWindowID;
            std::cout << "Found Test Window: " << name << " (WindowID: " << windowID << ")" << std::endl;
            break;
        }
    }

    MaaToolkitDesktopWindowListDestroy(winlist);

    if (windowID == 0) {
        std::cout << "未找到测试窗口，无法创建 MaaController。" << std::endl;
        exit(-1);
    }

    // 创建控制器
    // auto controller = MaaMacOSControllerCreate(windowID, MaaMacOSScreencapMethod_ScreenCaptureKit, MaaMacOSInputMethod_GlobalEvent);
    auto controller = MaaMacOSControllerCreate(windowID, MaaMacOSScreencapMethod_ScreenCaptureKit, MaaMacOSInputMethod_PostToPid);
    if (!controller) {
        std::cout << "创建 MaaController 失败。" << std::endl;
        exit(-1);
    }

    std::cout << "MaaController 创建成功，开始测试输入..." << std::endl;

    // 连接控制器
    auto ctrl_id = MaaControllerPostConnection(controller);

    // 创建资源
    auto resource_handle = MaaResourceCreate();
    std::string resource_dir = exe_dir + "../../resources/macos_test";
    auto res_id = MaaResourcePostBundle(resource_handle, resource_dir.c_str());

    MaaControllerWait(controller, ctrl_id);
    MaaResourceWait(resource_handle, res_id);

    // 创建任务器
    auto tasker_handle = MaaTaskerCreate();
    MaaTaskerBindResource(tasker_handle, resource_handle);
    MaaTaskerBindController(tasker_handle, controller);

    auto destroy = [&]() {
        MaaTaskerDestroy(tasker_handle);
        MaaResourceDestroy(resource_handle);
        MaaControllerDestroy(controller);
    };

    if (!MaaTaskerInited(tasker_handle)) {
        std::cout << "Failed to init MAA" << std::endl;
        destroy();
        return;
    }

    // 执行任务
    auto task_id = MaaTaskerPostTask(tasker_handle, "test_start", "{}");
    MaaTaskerWait(tasker_handle, task_id);

    std::cout << "MAA 任务执行完成。" << std::endl;

    destroy();
}

pid_t launchGUIProcess(const std::string& windowTitle, const std::string& exe_dir)
{
    std::string gui_exe = exe_dir + "macos_test_gui";

    pid_t pid = 0;
    char* argv[] = { const_cast<char*>(gui_exe.c_str()), const_cast<char*>(windowTitle.c_str()), nullptr };
    extern char** environ;
    int ret = posix_spawn(&pid, gui_exe.c_str(), nullptr, nullptr, argv, environ);
    if (ret != 0) {
        std::cerr << "Failed to launch GUI process: " << strerror(ret) << std::endl;
        return -1;
    }

    std::cout << "Launched GUI process (PID: " << pid << ")" << std::endl;
    return pid;
}

int main()
{
    checkAndRequestPermissions();

    uint32_t buf_size = 0;
    _NSGetExecutablePath(nullptr, &buf_size);
    std::string exe_path(buf_size, '\0');
    _NSGetExecutablePath(exe_path.data(), &buf_size);
    auto sep = std::string(exe_path.c_str()).rfind('/');
    std::string exe_dir = (sep != std::string::npos ? std::string(exe_path.c_str()).substr(0, sep + 1) : "./");

    std::string randomTitle = "MaaTestWindow_" + std::to_string(rand() % 10000);

    // 启动 GUI 进程
    pid_t gui_pid = launchGUIProcess(randomTitle, exe_dir);
    if (gui_pid <= 0) {
        std::cerr << "Failed to launch GUI process." << std::endl;
        return -1;
    }

    // 等待GUI进程启动并创建窗口
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 运行 MaaFW 测试
    runMaaTest(randomTitle, exe_dir);

    // 等待2S
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // 测试完成后终止 GUI 进程
    kill(gui_pid, SIGTERM);
    int status = 0;
    waitpid(gui_pid, &status, 0);
    std::cout << "GUI process terminated." << std::endl;

    return 0;
}
