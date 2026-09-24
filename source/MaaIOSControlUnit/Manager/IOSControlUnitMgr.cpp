#include "IOSControlUnitMgr.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <MaaUtils/Logger.h>
#include <MaaUtils/NoWarningCV.hpp>

#include "hid/Hid.h"

MAA_CTRL_UNIT_NS_BEGIN

namespace
{

constexpr int kDefaultHoldMs = 90;
/// 一次触摸事件之间至少留出的间隔。设备侧靠帧间差值算速度，瞬移式的拖动会被
/// 当成抖动丢掉，所以 duration 再短也要按这个间隔铺开若干步。
constexpr int kMinStepMs = 12;

/// Android keycode -> iOS Consumer page usage。只映射 iOS 上真有的那几个硬件键；
/// 其余（方向键、菜单键…）在 iOS 上没有对应物，返回 0 让调用方报错。
uint16_t consumer_usage_for(int keycode)
{
    switch (keycode) {
    case 3: // KEYCODE_HOME
        return scrctl::hid::button::kHome;
    case 26: // KEYCODE_POWER
        return scrctl::hid::button::kLock;
    case 24: // KEYCODE_VOLUME_UP
        return scrctl::hid::button::kVolumeUp;
    case 25: // KEYCODE_VOLUME_DOWN
        return scrctl::hid::button::kVolumeDown;
    case 164: // KEYCODE_VOLUME_MUTE
        return scrctl::hid::button::kMute;
    default:
        return 0;
    }
}

} // namespace

IOSControlUnitMgr::IOSControlUnitMgr(std::string udid)
    : udid_(std::move(udid))
{
}

IOSControlUnitMgr::~IOSControlUnitMgr()
{
    session_.close();
}

bool IOSControlUnitMgr::connect()
{
    std::string err;
    if (!session_.create(udid_, err)) {
        LogError << "Failed to create iOS session" << VAR(err);
        return false;
    }

    display_width_ = 0;
    display_height_ = 0;

    // 顺手把显示尺寸取到：不取的话第一次触摸之前会多一次截图的开销，
    // 而 connect() 本来就要建立整条隧道，多这一张不心疼。
    std::string size_err;
    if (!refresh_display_size(size_err)) {
        LogWarn << "connect ok but display size unknown, will retry on first touch" << VAR(size_err);
    }
    return true;
}

bool IOSControlUnitMgr::connected() const
{
    return session_.connected();
}

bool IOSControlUnitMgr::request_uuid(std::string& uuid)
{
    uuid = session_.udid();
    return !uuid.empty();
}

MaaControllerFeature IOSControlUnitMgr::get_features() const
{
    // click/swipe 直接实现（tap / 插值拖动），touch_down/up 也都原生支持，
    // 不需要框架改用 down+up 拼。坐标缩放开着：框架负责把用户看到的
    // 目标尺寸换算回截图原始像素，我们只按原始像素归一化。
    return MaaControllerFeature_None;
}

bool IOSControlUnitMgr::start_app(const std::string& intent)
{
    // intent 在这里就是 bundle id（com.apple.mobilesafari 这种），和 Android 那边
    // 传包名是同一个位置。语义也对齐：Android 用 `monkey -p <pkg> 1` 唤起，不动
    // 已经在跑的实例，所以这边 terminateExisting 传 false。
    std::string err;
    if (!session_.launch_app(intent, err)) {
        LogError << "start_app failed" << VAR(intent) << VAR(err);
        return false;
    }
    return true;
}

bool IOSControlUnitMgr::stop_app(const std::string& intent)
{
    std::string err;
    if (!session_.stop_app(intent, err)) {
        LogError << "stop_app failed" << VAR(intent) << VAR(err);
        return false;
    }
    return true;
}

bool IOSControlUnitMgr::refresh_display_size(std::string& err)
{
    cv::Mat image;
    if (!session_.screencap(image, err)) {
        return false;
    }
    display_width_ = image.cols;
    display_height_ = image.rows;
    LogInfo << "iOS display size" << VAR(display_width_) << VAR(display_height_);
    return true;
}

bool IOSControlUnitMgr::normalize(int x, int y, double& fx, double& fy, std::string& err)
{
    if (display_width_ <= 0 || display_height_ <= 0) {
        if (!refresh_display_size(err)) {
            LogError << "cannot learn display size" << VAR(err);
            return false;
        }
    }
    // 出界只警告不报错：坐标来自框架，正常路径不会越界，真越界了也照样夹进
    // 触摸面（设备侧本来就会夹），但要把尺寸对不上的事实留在日志里。
    if (x < 0 || y < 0 || x >= display_width_ || y >= display_height_) {
        LogWarn << "touch point outside the screenshot" << VAR(x) << VAR(y) << VAR(display_width_)
                << VAR(display_height_);
    }
    fx = std::clamp(static_cast<double>(x) / display_width_, 0.0, 1.0);
    fy = std::clamp(static_cast<double>(y) / display_height_, 0.0, 1.0);
    return true;
}

bool IOSControlUnitMgr::screencap(cv::Mat& image)
{
    std::string err;
    if (!session_.screencap(image, err)) {
        LogError << "screencap failed" << VAR(err);
        return false;
    }

    display_width_ = image.cols;
    display_height_ = image.rows;
    return true;
}

bool IOSControlUnitMgr::click(int x, int y)
{
    std::string err;
    double fx = 0, fy = 0;
    if (!normalize(x, y, fx, fy, err)) {
        LogError << "click failed to normalize" << VAR(x) << VAR(y) << VAR(err);
        return false;
    }
    if (!session_.tap(fx, fy, kDefaultHoldMs, err)) {
        LogError << "tap failed" << VAR(fx) << VAR(fy) << VAR(err);
        return false;
    }
    return true;
}

bool IOSControlUnitMgr::swipe(int x1, int y1, int x2, int y2, int duration)
{
    std::string err;
    double fx1 = 0, fy1 = 0, fx2 = 0, fy2 = 0;
    if (!normalize(x1, y1, fx1, fy1, err) || !normalize(x2, y2, fx2, fy2, err)) {
        LogError << "swipe failed to normalize" << VAR(x1) << VAR(y1) << VAR(x2) << VAR(y2) << VAR(err);
        return false;
    }

    const int steps = std::max(2, duration / kMinStepMs);
    std::vector<std::pair<double, double>> points;
    points.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        points.emplace_back(fx1 + (fx2 - fx1) * t, fy1 + (fy2 - fy1) * t);
    }

    const int step_ms = steps > 0 ? std::max(1, duration / steps) : kMinStepMs;
    if (!session_.stroke(points, step_ms, err)) {
        LogError << "stroke failed" << VAR(x1) << VAR(y1) << VAR(x2) << VAR(y2) << VAR(duration) << VAR(err);
        return false;
    }
    return true;
}

bool IOSControlUnitMgr::touch_down(int contact, int x, int y, int pressure)
{
    (void)pressure;
    if (contact != 0) {
        // 设备的 mainTouchscreen 报告只有一个接触点的位置字段，多指得换
        // 报告布局（保留区里大概藏着多点数据，但没逆出来）。
        LogError << "iOS controller supports single touch only" << VAR(contact);
        return false;
    }

    std::string err;
    if (!normalize(x, y, last_touch_.first, last_touch_.second, err)) {
        return false;
    }
    return session_.touch(last_touch_.first, last_touch_.second, true, err);
}

bool IOSControlUnitMgr::touch_move(int contact, int x, int y, int pressure)
{
    (void)pressure;
    if (contact != 0) {
        LogError << "iOS controller supports single touch only" << VAR(contact);
        return false;
    }

    std::string err;
    if (!normalize(x, y, last_touch_.first, last_touch_.second, err)) {
        return false;
    }
    // 每个 CONTACT 报告都是"此刻在此处接触着"，没有 begin/end 操作码，
    // 所以 move 与 down 在线上一模一样。
    return session_.touch(last_touch_.first, last_touch_.second, true, err);
}

bool IOSControlUnitMgr::touch_up(int contact)
{
    if (contact != 0) {
        LogError << "iOS controller supports single touch only" << VAR(contact);
        return false;
    }

    std::string err;
    return session_.touch(last_touch_.first, last_touch_.second, false, err);
}

bool IOSControlUnitMgr::click_key(int key)
{
    const uint16_t usage = consumer_usage_for(key);
    if (usage == 0) {
        LogError << "No iOS hardware button for this keycode" << VAR(key);
        return false;
    }

    std::string err;
    if (!session_.press_button(usage, err)) {
        LogError << "press button failed" << VAR(key) << VAR(err);
        return false;
    }
    return true;
}

bool IOSControlUnitMgr::input_text(const std::string& text)
{
    std::string err;
    if (!session_.type_text(text, err)) {
        LogError << "input_text failed" << VAR(err);
        return false;
    }
    return true;
}

bool IOSControlUnitMgr::key_down(int key)
{
    (void)key;
    // 硬件按键没有"按住不放"的语义可用：indigo 的 button 是 down/up 一组成对。
    LogWarn << "key_down not supported on iOS controller";
    return false;
}

bool IOSControlUnitMgr::key_up(int key)
{
    (void)key;
    LogWarn << "key_up not supported on iOS controller";
    return false;
}

bool IOSControlUnitMgr::inactive()
{
    return true;
}

json::object IOSControlUnitMgr::get_info() const
{
    json::object info;
    info["udid"] = session_.udid();
    info["product_type"] = session_.device_property("ProductType");
    info["os_version"] = session_.device_property("OSVersion");
    info["market_name"] = session_.device_property("MarketName");
    info["display_width"] = display_width_;
    info["display_height"] = display_height_;
    return info;
}

MAA_CTRL_UNIT_NS_END
