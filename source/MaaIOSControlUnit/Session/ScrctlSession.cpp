#include "ScrctlSession.h"

#include <chrono>
#include <thread>

#include <MaaUtils/Logger.h>
#include <MaaUtils/NoWarningCV.hpp>
#include <opencv2/imgproc.hpp>

#include "hid/Hid.h"
#include "media/FramePump.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace maa::ios_unit
{

namespace
{

constexpr int kFrameWaitMs = 3000;
constexpr int kFreshFrameWaitMs = 800;

} // namespace

ScrctlSession::ScrctlSession() = default;

ScrctlSession::~ScrctlSession()
{
    close();
}

bool ScrctlSession::create(const std::string& udid, std::string& err)
{
    if (device_) {
        return true;
    }

    auto device = scrctl::remote::Device::establish(udid, err);
    if (!device) {
        LogError << "CoreDevice session failed" << VAR(err);
        return false;
    }
    device_ = std::make_unique<scrctl::remote::Device>(std::move(*device));
    udid_ = device_->udid();

    scrctl::media::FramePump::Options pump_options;
    pump_ = scrctl::media::FramePump::start(*device_, pump_options, err);
    if (!pump_) {
        LogError << "start media stream failed" << VAR(err);
        device_.reset();
        return false;
    }

    scrctl::Frame first;
    if (!pump_->latest(first, kFrameWaitMs)) {
        err = "no frame decoded within " + std::to_string(kFrameWaitMs) + "ms";
        LogError << err;
        close();
        return false;
    }

    // 面的认证状态是流起来之后才翻的，立刻发报告会被丢掉。
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    hid_ = scrctl::hid::Service::open(*device_, err);
    if (!hid_) {
        LogError << "open universalhidservice failed" << VAR(err);
        close();
        return false;
    }

    LogInfo << "iOS session ready" << scrctl::remote::mask(udid_)
            << VAR(device_->property("ProductType")) << VAR(device_->property("OSVersion"))
            << VAR(first.width) << VAR(first.height);
    return true;
}

void ScrctlSession::close()
{
    hid_.reset();
    buttons_.reset();
    // 泵先停：它拥有那条会话，析构时会调 stopmediastream。
    pump_.reset();
    device_.reset();
    udid_.clear();
    last_serial_ = 0;
}

bool ScrctlSession::convert(const scrctl::Frame& frame, cv::Mat& image)
{
    if (!frame) {
        return false;
    }

    const int coded_w = static_cast<int>(frame.width);
    const int coded_h = static_cast<int>(frame.height);
    // row_pitch 可能大于 width*4，必须按行构造而不是当成连续内存。
    cv::Mat bgra(coded_h, coded_w, CV_8UC4, const_cast<uint8_t*>(frame.pixels.data()),
                 frame.row_pitch);

    const auto crop = scrctl::media::display_crop(coded_w, coded_h);
    // 裁剪框必须落在帧内：认不出的分辨率会原样返回，但用户给的范围未必自洽。
    if (crop.x < 0 || crop.y < 0 || crop.x + crop.w > coded_w || crop.y + crop.h > coded_h) {
        LogError << "invalid crop" << VAR(crop.x) << VAR(crop.y) << VAR(crop.w) << VAR(crop.h)
                 << VAR(coded_w) << VAR(coded_h);
        return false;
    }

    cv::Mat cropped = bgra(cv::Rect(crop.x, crop.y, crop.w, crop.h));
    cv::cvtColor(cropped, image, cv::COLOR_BGRA2BGR);
    return !image.empty();
}

bool ScrctlSession::screencap(cv::Mat& image, std::string& err)
{
    if (!pump_) {
        err = "not connected";
        return false;
    }

    scrctl::Frame frame;
    if (uint64_t serial = pump_->newer(frame, last_serial_, kFreshFrameWaitMs)) {
        last_serial_ = serial;
        if (convert(frame, image)) {
            return true;
        }
    }

    // 等不到新帧有两种情况：屏幕本来就静止（正常），或流卡住了（要兜底）。
    // 分不清就都走同一条路：先拿"最新一帧"，再不行才退回截图 RPC。
    if (pump_->latest(frame, kFrameWaitMs) && convert(frame, image)) {
        return true;
    }

    LogWarn << "frame pump gave nothing, falling back to screencapture service";
    auto input = scrctl::xpc::make_dict();
    scrctl::xpc::dict_set(input, "displayUniqueID", scrctl::xpc::make_null());
    scrctl::xpc::dict_set(input, "requestedFormat", scrctl::xpc::make_string("png"));

    scrctl::xpc::Value output;
    if (!device_->feature("com.apple.coredevice.screencaptureservice",
                          "com.apple.coredevice.feature.capturescreenshot",
                          "com.apple.coredevice.action.capturescreenshot", input, output, err,
                          false, 30000)) {
        LogError << "capturescreenshot failed" << VAR(err);
        return false;
    }
    const auto* png = output.find("image");
    if (png == nullptr || png->data.empty()) {
        err = "screenshot reply has no image bytes";
        LogError << err;
        return false;
    }

    image = cv::imdecode({ png->data.data(), static_cast<int>(png->data.size()) }, cv::IMREAD_COLOR);
    if (image.empty()) {
        err = "failed to decode screenshot PNG";
        return false;
    }
    return true;
}

bool ScrctlSession::touch(double x, double y, bool down, std::string& err)
{
    if (!hid_) {
        err = "not connected";
        return false;
    }
    return hid_->touch(scrctl::hid::kSurfaceMainTouchscreen, x, y, down, err);
}

bool ScrctlSession::tap(double x, double y, int hold_ms, std::string& err)
{
    if (!hid_) {
        err = "not connected";
        return false;
    }
    return hid_->tap(x, y, hold_ms, err);
}

bool ScrctlSession::stroke(const std::vector<std::pair<double, double>>& points, int step_ms,
                           std::string& err)
{
    if (!hid_) {
        err = "not connected";
        return false;
    }
    return hid_->stroke(points, step_ms, err);
}

bool ScrctlSession::type_text(const std::string& text, std::string& err)
{
    if (!hid_) {
        err = "not connected";
        return false;
    }
    const auto reports = scrctl::hid::text_reports(text);
    if (reports.empty()) {
        // 一个都翻不出来比"打了个空字符串"更该报错：调用方以为自己输入过了，
        // 而识别层接下来看到的是没变过的界面。
        err = "no inputable character in \"" + text + "\" (only US-layout ASCII is supported)";
        LogError << err;
        return false;
    }
    if (reports.size() < text.size() * 2) {
        LogWarn << "some characters were skipped" << VAR(text.size()) << VAR(reports.size());
    }
    return hid_->type_text(text, 40, err);
}

bool ScrctlSession::press_button(uint16_t usage_code, std::string& err)
{
    if (!device_) {
        err = "not connected";
        return false;
    }
    if (!buttons_) {
        buttons_ = scrctl::hid::Buttons::open(*device_, err);
        if (!buttons_) {
            LogError << "open hid.indigo failed" << VAR(err);
            return false;
        }
    }
    return buttons_->press(scrctl::hid::button::kUsagePageConsumer, usage_code, 90, err);
}

std::string ScrctlSession::device_property(const char* key) const
{
    return device_ ? device_->property(key) : std::string();
}

} // namespace maa::ios_unit
