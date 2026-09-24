#include "ScrctlSession.h"

#include <chrono>
#include <thread>

#include <MaaUtils/Logger.h>
#include <MaaUtils/NoWarningCV.hpp>
#include <opencv2/imgproc.hpp>

#include "hid/Hid.h"
#include "media/FramePump.h"
#include "remote/App.h"
#include "remote/Device.h"
#include "xpc/XpcValue.h"

namespace maa::ios_unit
{

namespace
{

constexpr int kFrameWaitMs = 3000;
/// 第一段等帧预算。屏幕在动时一帧 16ms 就到，这个预算只在"流活着但画面静止到不发
/// 帧了"的那段窗口里付（实测是拆流前那约 7 秒）。取 300ms 还有一个作用：wake() 催
/// 出去之后，泵最多 50ms 就会把"要不要救流"定下来，而那条问设备的 RPC 要 100~300ms
/// ——等满 300ms 再读 reviving()，读到"还在救"结果其实答案是"活着不用救"的概率就很小了。
constexpr int kFirstFrameWaitMs = 300;
/// 泵正在救流时再多给的时间。为什么要有第二段：**"等不到新帧"有两种完全不同的
/// 原因**——救流还在路上（再等就有），或者屏幕本来就静止（再等多久都没有，而最新
/// 一帧就是当前画面）。只看"有没有新帧"分不出这两种，用同一个超时必然一边太短一边
/// 太长。重起到第一帧实测 189~249ms（8 次，scrctl/tools/wake_latency_probe --quiet
/// 1500 与 4000 两档），但停+起那两条 RPC 偶尔慢到近一秒，所以这一段给到 3 秒。
constexpr int kReviveWaitMs = 3000;

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
    // 关掉"静默满 3 秒就自己去重起"。那是给窗口镜像用的（有人盯着，画面必须自己
    // 回来）；自动化是拉模型，没人截图的时候根本不需要帧。留着它的后果是：只要会话
    // 开着，就会按"设备 6.9 秒拆流 -> 泵 3 秒后重起"的节拍每约 10 秒对设备做一次
    // 停+起，手机白烧电与带宽，而我们一帧都不看。改由 screencap 里的 wake() 按需催。
    pump_options.silence_restart_ms = 0;
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

    // 先记下"此刻泵手里最新的一帧是第几帧"，再催流。顺序不能反，而且这个起点必须是
    // "现在最新的那一帧"而不是"我上次取走的那一帧"：设备会在画面静止一会儿之后把整条
    // 会话结束掉（实测最后一个视频包之后约 6.9 秒），而会话死了之后设备上任何画面变化
    // 都不会再推过来——于是泵手里那张"最新"的帧其实是拆流前那一刻的旧画面。拿"我上次
    // 看过的"当起点，这张旧帧只要比它新就会被当成新帧交出去（实测：截图耗时 3ms，内容
    // 与动作前逐像素一致），对自动化来说这是最坏的一种错，因为它返回真、看着是成功的。
    const uint64_t floor_serial = pump_->serial();
    // 心跳还在时 wake() 是空操作，所以每次截图都催得起。去掉这一句的话，"静置 20 秒
    // -> swipe -> 截图"实测会一直交出拆流前那张旧图。
    pump_->wake();

    scrctl::Frame frame;
    uint64_t serial = pump_->newer(frame, floor_serial, kFirstFrameWaitMs);
    if (serial == 0 && pump_->reviving()) {
        // 泵正在救这条流：这一帧一定会来，多等一会儿，而不是把旧的那张交出去
        serial = pump_->newer(frame, floor_serial, kReviveWaitMs);
    }
    if (serial != 0 && convert(frame, image)) {
        return true;
    }

    // 没在救流又等不到新帧：流活着而画面本来就静止——"最新一帧"就是当前画面，
    // 交出去是对的。再拿不到才退回截图 RPC。
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

bool ScrctlSession::launch_app(const std::string& bundle_id, std::string& err)
{
    if (!device_) {
        err = "not connected";
        return false;
    }
    if (!scrctl::remote::App::launch(*device_, bundle_id, err, /*terminate_existing = */ false)) {
        LogError << "launchapplication failed" << VAR(bundle_id) << VAR(err);
        return false;
    }
    LogInfo << "app launched" << bundle_id;
    return true;
}

} // namespace maa::ios_unit
