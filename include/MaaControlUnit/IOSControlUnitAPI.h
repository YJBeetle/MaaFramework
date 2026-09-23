#pragma once

#include <memory>
#include <optional>

#include "MaaControlUnit/ControlUnitAPI.h"
#include "MaaFramework/MaaDef.h"

#ifdef __cplusplus
extern "C"
{
#endif

    MAA_CONTROL_UNIT_API const char* MaaIOSControlUnitGetVersion();

    /**
     * @brief Create an iOS device control unit talking over the CoreDevice/DDI stack.
     *
     * @param udid The device UDID to connect, or NULL/"" to use the only connected device.
     * @return The control unit handle, or nullptr on failure.
     *
     * @note The device must be paired and have a Developer Disk Image mounted
     *       (Xcode does this automatically when a device is registered).
     * @note Coordinates are in screenshot pixel space; the unit normalizes them
     *       to the device's 0..1 touch surface internally.
     * @note Not supported: start_app, stop_app, input_text, key_down/key_up,
     *       relative_move. `click_key` only maps to hardware buttons.
     */
    MAA_CONTROL_UNIT_API MaaIOSControlUnitHandle MaaIOSControlUnitCreate(const char* udid);

    MAA_CONTROL_UNIT_API void MaaIOSControlUnitDestroy(MaaIOSControlUnitHandle handle);

#ifdef __cplusplus
}
#endif
