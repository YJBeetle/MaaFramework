#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

#include "MaaFramework/MaaDef.h"

namespace scrctl
{
struct Frame;
}

namespace scrctl::media
{
class FramePump;
}

namespace scrctl::remote
{
class Device;
}

namespace scrctl::hid
{
class Service;
class Buttons;
}

namespace maa::ios_unit
{

/// 一台设备的一次完整会话：CoreDevice 隧道 + 视频流取帧 + HID 注入通道。
///
/// 这是 scrctl 与 MaaFramework 之间唯一的边界：上面这层只讲"截图 / 点 / 滑 /
/// 按键"，不讲 XPC、RTP 也不讲 HID 报告。
///
/// 截图有两条路：视频流的最新一帧，和 capturescreenshot RPC。前者实测中位 14~17ms，
/// 后者 113~891ms —— Maa 的整个循环是"截图 -> 识别 -> 动作"，这个差别决定任务能跑多快。
/// 但视频流是有损的、要常驻解码，所以选哪条（或两条都给、坏了自动降级）交给调用方，
/// 见 create() 的 `screencap_methods`。
class ScrctlSession {
public:
    ScrctlSession();
    ~ScrctlSession();

    ScrctlSession(const ScrctlSession&) = delete;
    ScrctlSession& operator=(const ScrctlSession&) = delete;

    /// 建立会话。udid 为空表示"恰好一台就用它"，多台时报错并列出候选。
    ///
    /// `screencap_methods` 决定取图用哪几条路（见 MaaIOScreencapMethod）。下面的数都是
    /// 2026-09-27 在 iPhone14,4 / iOS 27 / USB、设备上只有我们这个客户端时量的。
    /// - 带 Stream：起一条媒体流当快路径，中位 14~17ms/张（列表、空板、涂满的板都是这一档，
    ///   静置 90 秒后首张 14ms）。代价是后台常驻解码和有损图。会话不会自己断——那条租期是
    ///   我们自己报的（scrctl 现在报 3600 秒），而且泵每秒发一次 RTCP RR 续着它。
    /// - 只带 ScreenshotService：**根本不建媒体会话、不起泵**，每次截图一条
    ///   capturescreenshot RPC。换来零后台解码和无损图，代价是慢 10~40 倍。
    ///   ⚠ 这个"慢"没有单一倍数，别拿一个中位数当常数：量的三块画面分别是
    ///   167ms（无边记看板列表）、743ms（涂了 40 条粗马克笔的板）、518ms（画了 140 条
    ///   细线的密网板）。密网板亮点是粗笔板的 4 倍却更快，所以驱动量是设备那边 PNG 编出来
    ///   多大，而不是"屏幕看着多满"；这层关系还没测清，先别写定律。
    /// - 两个都给就是"快的优先、坏了自动降级"，这是默认。降级不是免费的：泵要连续 3 次重起
    ///   才把 video_unusable 锁上，锁上之前每次调用都先付满等帧预算，所以同一块解不了的密网
    ///   板上 both 中位 860ms、而只给服务是 518ms。锁上之后就直奔服务，不再付等帧钱。
    /// - 只给 Stream 则没有退路：单帧超过解码后端上限的画面（实测那块密网板 IDR 256278 字节）
    ///   每一张都失败并给出明确错误，而不是偷偷变慢；这种画面下 connect 本身要 7 秒（一直在
    ///   重起）。
    ///
    /// 它**不是**触摸注入的前提。早先 scrctl 里记着"没有流在跑时设备把 HID 面标成
    /// 未认证、backboardd 会静默丢掉每个触摸事件"，2026-09-25 用
    /// scrctl/tools/hid_gate_probe 复测推翻了：会话停掉之后、我们主动拆掉会话之后、
    /// 甚至全新进程一次流都没起过，注入实测都照样落地。所以下面几个
    /// 输入方法都不催流（见 scrctl/docs/coredevice.md §11）。
    bool create(const std::string& udid, MaaIOScreencapMethod screencap_methods, std::string& err);

    void close();

    [[nodiscard]] bool connected() const { return device_ != nullptr; }

    /// BGR 三通道图，已裁掉 HEVC 的 CTU 填充，尺寸就是逻辑显示尺寸。
    bool screencap(cv::Mat& image, std::string& err);

    /// 坐标是"整块屏幕的 0..1"。设备侧的触摸面本来就是归一化的，所以这条边界上
    /// 传分数比传像素好——换机型不用改任何东西。
    bool touch(double x, double y, bool down, std::string& err);
    bool tap(double x, double y, int hold_ms, std::string& err);
    bool stroke(const std::vector<std::pair<double, double>>& points, int step_ms, std::string& err);
    bool press_button(uint16_t usage_code, std::string& err);

    /// 往设备敲一段文本。前提是有文本框正获得焦点。
    ///
    /// 走的是设备自带的键盘面（_ServiceID 512），不需要宿主注册虚拟键盘——实测
    /// 报告直接落进焦点框。只覆盖 US 布局上能按出来的 ASCII；认不出的字符会被
    /// 跳过，所以中文要另想办法（剪贴板）。
    bool type_text(const std::string& text, std::string& err);

    [[nodiscard]] const std::string& udid() const { return udid_; }
    [[nodiscard]] std::string device_property(const char* key) const;

    /// 把一个 App 带到前台。**不动已经在跑的实例**——Android 那边 start_app 是
    /// `monkey -p <pkg> 1`，语义就是"唤起/恢复"，不是冷启动。
    ///
    /// 别改成 terminateExisting=true：实测那样会先把实例杀掉，而杀掉之后设备有概率
    /// 回 "The process identifier of the launched application could not be determined"
    /// （code 10004）——**这句失败是"已经杀了、没起来"**，调用方拿到 false 时前台 App
    /// 已经没了，比不动还糟。
    bool launch_app(const std::string& bundle_id, std::string& err);

    /// 杀掉一个 App（SIGKILL 给它的所有进程）。App 本来没在跑时返回真——Android 那边
    /// `am force-stop` 也是这个语义，调用方的意图（"它别在跑"）已经成立。
    bool stop_app(const std::string& bundle_id, std::string& err);

private:
    /// 把一帧 BGRA 变成裁好、转好色的 BGR。
    static bool convert(const scrctl::Frame& frame, cv::Mat& image);

    /// 不经过视频流问一次截图服务（`capturescreenshot`）。它自己走 CoreDevice 的 RPC
    /// 通道，所以视频流死了、甚至这条流根本解不了的时候它照样能拿到当前画面——代价是
    /// 一次 RPC 加一张 PNG 解码，比从泵里取一帧慢一个数量级。
    bool screencap_via_service(cv::Mat& image, std::string& err);

    std::unique_ptr<scrctl::remote::Device> device_;
    std::unique_ptr<scrctl::media::FramePump> pump_;
    std::unique_ptr<scrctl::hid::Service> hid_;
    std::unique_ptr<scrctl::hid::Buttons> buttons_;
    std::string udid_;
    /// create() 时拿到的那张位掩码，screencap() 每次照它选路。泵为空不等于没连上，
    /// 也可能是调用方压根不要视频路，所以这两件事要分开记。
    MaaIOScreencapMethod screencap_methods_ = MaaIOScreencapMethod_Default;
};

} // namespace maa::ios_unit
