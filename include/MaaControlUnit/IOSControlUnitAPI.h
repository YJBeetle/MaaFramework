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
     * @param screencap_methods Bitmask of allowed screencap methods. With only
     *       MaaIOScreencapMethod_ScreenshotService set, no media stream is ever started
     *       (nothing decodes in the background); with only _Stream set there is no
     *       fallback, so a screen the decoder cannot handle fails instead of degrading.
     * @return The control unit handle, or nullptr on failure.
     *
     * @note The device must be paired and have a Developer Disk Image mounted
     *       (Xcode does this automatically when a device is registered).
     * @note Coordinates are in screenshot pixel space; the unit normalizes them
     *       to the device's 0..1 touch surface internally.
     * @note Not supported: key_down/key_up, relative_move.
     *       `click_key` only maps to hardware buttons (home, volume, lock...).
     * @note start_app / stop_app take a bundle identifier. start_app brings the
     *       app to the foreground without disturbing a running instance -- same
     *       semantics as the Android unit (`monkey -p <pkg> 1`); stop_app SIGKILLs
     *       every process whose executable lives in the app's bundle, and succeeds
     *       when the app was not running (like `am force-stop`).
     * @note input_text covers ASCII reachable on the US layout; it returns false
     *       when nothing in the string can be typed rather than silently sending
     *       an empty keystroke sequence.
     */
    MAA_CONTROL_UNIT_API MaaIOSControlUnitHandle
        MaaIOSControlUnitCreate(const char* udid, MaaIOScreencapMethod screencap_methods);

    MAA_CONTROL_UNIT_API void MaaIOSControlUnitDestroy(MaaIOSControlUnitHandle handle);

#ifdef __cplusplus
}
#endif
