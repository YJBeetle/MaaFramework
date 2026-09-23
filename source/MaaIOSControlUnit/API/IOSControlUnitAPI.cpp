#include "MaaControlUnit/IOSControlUnitAPI.h"

#include <MaaUtils/Logger.h>

#include "Manager/IOSControlUnitMgr.h"

extern "C" {

const char* MaaIOSControlUnitGetVersion()
{
#pragma message("MaaIOSControlUnit MAA_VERSION: " MAA_VERSION)

    return MAA_VERSION;
}

MaaIOSControlUnitHandle MaaIOSControlUnitCreate(const char* udid)
{
    using namespace MAA_CTRL_UNIT_NS;

    // 直接 VAR(udid) 会在 NULL 时把空指针交给 operator<<(const char*)，那是 UB。
    const std::string udid_str = udid ? udid : "";
    LogFunc << VAR(udid_str);

    auto unit_mgr = std::make_unique<IOSControlUnitMgr>(udid_str);
    return unit_mgr.release();
}

void MaaIOSControlUnitDestroy(MaaIOSControlUnitHandle handle)
{
    LogFunc << VAR_VOIDP(handle);

    if (handle) {
        delete handle;
    }
}

} // extern "C"
