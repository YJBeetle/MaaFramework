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
/// 第一段等帧预算。屏幕在动时一帧 16ms 就到，这个预算只在"流活着但画面静止到不发帧
/// 了"的那段窗口里付：静止画面上设备一个视频包都不发（只剩每秒那个 SR 心跳）。这段
/// 窗口现在可以很长——会话的租期是我们在请求里报的（scrctl 报 3600 秒），过去那个数
/// 是 20 秒，所以静止窗口最长也就 20 秒减去起流到静止的那一段。
/// 取 300ms 还有一个作用：wake() 催出去之后，泵最多 50ms 就会把"要不要救流"定下来，
/// 而那条问设备的 RPC 要 100~300ms——等满 300ms 再读 reviving()，读到"还在救"结果
/// 其实答案是"活着不用救"的概率就很小了。
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

bool ScrctlSession::create(const std::string& udid, MaaIOScreencapMethod screencap_methods, std::string& err)
{
    if (device_) {
        return true;
    }

    if (screencap_methods == MaaIOScreencapMethod_None) {
        err = "no screencap method selected";
        LogError << err;
        return false;
    }
    screencap_methods_ = screencap_methods;

    auto device = scrctl::remote::Device::establish(udid, err);
    if (!device) {
        LogError << "CoreDevice session failed" << VAR(err);
        return false;
    }
    device_ = std::make_unique<scrctl::remote::Device>(std::move(*device));
    udid_ = device_->udid();

    scrctl::Frame first;
    const bool want_stream = (screencap_methods_ & MaaIOScreencapMethod_Stream) != 0;

    if (!want_stream) {
        // 调用方明确不要视频路：那就一条媒体会话都不建，后台一个解码线程都不起。
        LogInfo << "media stream not selected, screencap will use the screenshot service only";
    } else {
        scrctl::media::FramePump::Options pump_options;
        // 关掉"静默满 3 秒就自己去重起"。那是给窗口镜像用的（有人盯着，画面必须自己
        // 回来）；自动化是拉模型，没人截图的时候根本不需要帧。留着它的后果是：只要会话
        // 开着，就会按"租期到点 -> 泵发现死了再重起"的节拍反复对设备做停+起，手机白烧电
        // 与带宽，而我们一帧都不看。改由 screencap 里的 wake() 按需催：那条路会先看申请
        // 的租期有没有过（FramePump 的 kSessionLeaseMs），过期就直接重起，不再花一条 RPC
        // 去问设备"还在吗"。
        //
        // 这个节拍现在远没那么碍事了：那条租期是我们在 startmediastream 请求里自己报的
        // `timeout`（scrctl 现在报 3600 秒），过去它等于 20 秒，所以"开着不用的会话每 20
        // 秒对设备做一次停+起"是真会发生的。
        pump_options.silence_restart_ms = 0;
        pump_ = scrctl::media::FramePump::start(*device_, pump_options, err);
        if (!pump_) {
            LogError << "start media stream failed" << VAR(err);
            device_.reset();
            return false;
        }

        if (!pump_->latest(first, kFrameWaitMs)) {
            // 拿不到第一帧**不算连接失败**。以前这里直接 return false，于是"画面复杂到
            // VideoToolbox 吃不下这一帧"（实测无边记画满白线的看板，IDR 256278 字节，超过
            // 2 字节长度前缀上限）会让整个控制单元连不上——而它其实每次截图都能拿到正确的图。
            // 三件事各自独立：输入通路不需要这条流（实测流死了触摸照样落地）；取图还有截图
            // 服务这条路（screencap 里的 video_unusable 快路径）；泵自己会退避重试并在解出
            // 帧时自动回到视频路。所以这里最坏的判断也只是"暂时只能用截图服务"。
            //
            // 但这条兜底只在调用方允许它的时候存在：只给 Stream 时没有退路，失败就该报出来。
            LogWarn << "no frame decoded within" << VAR(kFrameWaitMs)
                    << ", will use screencapture service until the video path works";
        }

        // 面的认证状态是流起来之后才翻的，立刻发报告会被丢掉。
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    hid_ = scrctl::hid::Service::open(*device_, err);
    if (!hid_) {
        LogError << "open universalhidservice failed" << VAR(err);
        close();
        return false;
    }

    LogInfo << "iOS session ready" << scrctl::remote::mask(udid_)
            << VAR(device_->property("ProductType")) << VAR(device_->property("OSVersion"))
            << VAR(screencap_methods_) << VAR(first.width) << VAR(first.height);
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
    if (!device_) {
        err = "not connected";
        return false;
    }

    const bool allow_service = (screencap_methods_ & MaaIOScreencapMethod_ScreenshotService) != 0;

    // 没有泵 = 调用方压根不要视频路；video_unusable = 要了但这条路当前走不通
    // （单帧大到解码后端吃不下，见 FramePump::video_unusable）。后者不必再花预算在等帧
    // 上，直接问截图服务；泵在后台偶尔还会重试，哪天真解出一帧这个判断就自己消失。
    if (!pump_ || pump_->video_unusable()) {
        if (!allow_service) {
            err = "no usable screencap method: video path unavailable and service not allowed";
            LogError << err << VAR(screencap_methods_) << VAR(pump_ != nullptr);
            return false;
        }
        return screencap_via_service(image, err);
    }

    // 先记下"此刻泵手里最新的一帧是第几帧"，再催流。顺序不能反，而且这个起点必须是
    // "现在最新的那一帧"而不是"我上次取走的那一帧"：会话一旦停了（租期到点、设备侧自己
    // 结束、屏幕被锁），停了之后设备上任何画面变化都不会再推过来——于是泵手里那张"最新"
    // 的帧其实是停流前那一刻的旧画面。拿"我上次
    // 看过的"当起点，这张旧帧只要比它新就会被当成新帧交出去（实测：截图耗时 3ms，内容
    // 与动作前逐像素一致），对自动化来说这是最坏的一种错，因为它返回真、看着是成功的。
    const uint64_t floor_serial = pump_->serial();
    // 同一时刻把泵的账也拍一张快照，事后用来分清"没有新帧"到底是哪种原因，见下面 starving。
    const scrctl::media::FramePump::Stats stats_before = pump_->stats();
    // 会话还新鲜（心跳还在）时 wake() 是空操作，所以每次截图都催得起。去掉
    // 这一句的话，"静置到流停 -> swipe -> 截图"实测会一直交出停流前那张旧图。租期已过
    // 时它走的是泵里那条"不问、直接重起"的快路。
    //
    // 注：这条租期是我们在 startmediastream 请求里自己报的数（scrctl 现在报 3600 秒），
    // 不再是过去那个 20 秒，所以"静置一会儿再截图"这种用法现在默认落在还活着的会话里。
    // wake() 依然要留着：它管的是"任何原因的停流"，而不只是租期到点。
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
    //
    // 但"等不到新帧"还有第三种原因，恰恰是 latest() 这条路最危险的漏：流活着、码流
    // 也一直进来，只是每一帧都被解码那侧丢掉了（单帧超过后端上限、丢了分片在等干净的
    // 关键帧、解了出不来图）。这时 serial() 不动而泵的账一直在涨，latest() 交出的是
    // **上一次成功解码那一刻**的画面——屏幕早就变了，返回值却是真的、看着成功。实测：
    // 从看板列表点进那块画满白线的画板，进板前后两次截图逐像素一致（都是列表，26336
    // 个亮点），而屏上其实已经是 187000 个亮点。
    //
    // 分得开，因为泵把每一层的丢弃都记了账：等帧这段时间里流水线的计数器涨过而帧号
    // 没涨，就说明"没新帧"是解不出来，不是屏幕静止，那张旧帧不能交。
    const scrctl::media::FramePump::Stats stats_after = pump_->stats();
    const auto pipeline_progress = [](const scrctl::media::FramePump::Stats& s) {
        return s.aus + s.decoded + s.no_output + s.dropped + s.dropped_oversized + s.dropped_awaiting_keyframe
               + s.dropped_fragments + s.gaps + s.restarts;
    };
    const bool starving = pipeline_progress(stats_after) > pipeline_progress(stats_before);

    // 另外泵**一个帧都没解出来过**时（serial()==0）这一步本来就没有意义：不是"画面静止
    // 所以没有新帧"，而是这条流根本还没产出过任何东西，再等 3 秒也是零。省掉它能把降级
    // 头两轮的单次截图从 ~6.9 秒压到 ~3.9 秒（实测：无边记那块解不了的画板上头两次
    // 截图各 6849/6894ms，之后泵自己判死、走快路径，稳定在 540-990ms）。
    if (!starving && pump_->serial() != 0 && pump_->latest(frame, kFrameWaitMs) && convert(frame, image)) {
        return true;
    }

    if (!allow_service) {
        // 只给了 Stream：没有退路，失败就报出来，而不是悄悄换成一条慢 40 倍的路。
        err = "media stream produced no frame and screencapture service is not allowed";
        LogError << err << VAR(screencap_methods_);
        return false;
    }

    LogWarn << "frame pump gave nothing, falling back to screencapture service" << VAR(starving)
            << VAR(floor_serial) << VAR(pump_->serial());
    return screencap_via_service(image, err);
}

bool ScrctlSession::screencap_via_service(cv::Mat& image, std::string& err)
{
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

bool ScrctlSession::stop_app(const std::string& bundle_id, std::string& err)
{
    if (!device_) {
        err = "not connected";
        return false;
    }
    if (!scrctl::remote::App::stop(*device_, bundle_id, err)) {
        LogError << "sendsignaltoprocess failed" << VAR(bundle_id) << VAR(err);
        return false;
    }
    LogInfo << "app stopped" << bundle_id;
    return true;
}

} // namespace maa::ios_unit
