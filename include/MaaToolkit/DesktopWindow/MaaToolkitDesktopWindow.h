// IWYU pragma: private, include <MaaToolkit/MaaToolkitAPI.h>

#pragma once

#include "../MaaToolkitDef.h"

#ifdef __cplusplus
extern "C"
{
#endif

    MAA_TOOLKIT_API MaaToolkitDesktopWindowList* MaaToolkitDesktopWindowListCreate();
    MAA_TOOLKIT_API void MaaToolkitDesktopWindowListDestroy(MaaToolkitDesktopWindowList* handle);

    MAA_TOOLKIT_API MaaBool MaaToolkitDesktopWindowFindAll(/*out*/ MaaToolkitDesktopWindowList* buffer);

    MAA_TOOLKIT_API MaaSize MaaToolkitDesktopWindowListSize(const MaaToolkitDesktopWindowList* list);
    MAA_TOOLKIT_API const MaaToolkitDesktopWindow* MaaToolkitDesktopWindowListAt(const MaaToolkitDesktopWindowList* list, MaaSize index);

    // Win32 字段
    MAA_TOOLKIT_API void* MaaToolkitDesktopWindowGetHandle(const MaaToolkitDesktopWindow* window);
    MAA_TOOLKIT_API const char* MaaToolkitDesktopWindowGetClassName(const MaaToolkitDesktopWindow* window);
    MAA_TOOLKIT_API const char* MaaToolkitDesktopWindowGetWindowName(const MaaToolkitDesktopWindow* window);

    // MacOS 字段
    MAA_TOOLKIT_API uint32_t MaaToolkitDesktopWindowGetWindowId(const MaaToolkitDesktopWindow* window);           // macOS 窗口ID
    MAA_TOOLKIT_API const char* MaaToolkitDesktopWindowGetTitle(const MaaToolkitDesktopWindow* window);           // macOS 窗口标题
    MAA_TOOLKIT_API int32_t MaaToolkitDesktopWindowGetPid(const MaaToolkitDesktopWindow* window);                 // macOS 进程ID
    MAA_TOOLKIT_API const char* MaaToolkitDesktopWindowGetBundleId(const MaaToolkitDesktopWindow* window);        // macOS Bundle ID
    MAA_TOOLKIT_API const char* MaaToolkitDesktopWindowGetApplicationName(const MaaToolkitDesktopWindow* window); // macOS 应用程序名称

#ifdef __cplusplus
}
#endif
