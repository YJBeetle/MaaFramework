#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

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
/// 截图走视频流而不是 screencaptureservice：实测取最新一帧中位 16.6ms，而截图
/// RPC 是 226ms —— Maa 的整个循环是"截图 -> 识别 -> 动作"，这个差别决定任务能
/// 跑多快。截图 RPC 留作兜底（流万一没帧可取）。
class ScrctlSession {
public:
    ScrctlSession();
    ~ScrctlSession();

    ScrctlSession(const ScrctlSession&) = delete;
    ScrctlSession& operator=(const ScrctlSession&) = delete;

    /// 建立会话。udid 为空表示"恰好一台就用它"，多台时报错并列出候选。
    ///
    /// 成功后**媒体流一直在跑**，它是"看现在屏幕上是什么"的唯一快路径：取最新一帧
    /// 中位 16.6ms，而截图 RPC 要 226ms。
    ///
    /// 它**不是**触摸注入的前提。早先 scrctl 里记着"没有流在跑时设备把 HID 面标成
    /// 未认证、backboardd 会静默丢掉每个触摸事件"，2026-09-25 用
    /// scrctl/tools/hid_gate_probe 复测推翻了：设备自己结束空闲会话之后、我们主动
    /// 拆掉会话之后、甚至全新进程一次流都没起过，注入实测都照样落地。所以下面几个
    /// 输入方法都不催流（见 scrctl/docs/coredevice.md §11）。
    bool create(const std::string& udid, std::string& err);

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
};

} // namespace maa::ios_unit
