# GStreamer H.265 错误掩盖、周期恢复与丢包 PSNR 实验

本项目是仓库内独立编译的 C++17 子项目，不修改原 H.264/GCC 工程。包括真实 UDP 收发
`h265_live`、可复现 RTP 丢包实验 `h265_resilience` 和报告生成器 `h265_report`。
未接入原 GCC 码率控制，实验固定编码目标码率以减少干扰。新增应用和指标算法均为
C++；PowerShell/Bash 仅用于下载及测试调度。GStreamer、x265、FFmpeg 为外部开源依赖。

已在本机 WSL2 Ubuntu / GCC 15.2 / GStreamer 1.28.2 软件编解码环境验证。
Windows 原生 GStreamer 和硬件解码器未验证。

## 已测结果

两段 Xiph 公共领域 720p 序列，分别 300 帧、60fps。以官方 WebM 解码画面作为
固定参考，非原始无损 Y4M。丢包率 0/0.1/0.5/1/2/5/10%，三个种子，六种模式，
合计 **252 组、75,600 个源帧计分槽**。

2% 丢包时 PSNR-Y（三种子均值）：

| 机制 | FourPeople | KristenAndSara |
|---|---:|---:|
| 严格解码，缺帧黑帧 | 8.28 dB | 8.11 dB |
| 容错解码，缺帧黑帧 | 12.13 dB | 11.27 dB |
| 容错解码 + 上一帧保持 | 12.81 dB | 11.73 dB |
| 4 slices + 上一帧保持 | 13.88 dB | 13.32 dB |
| 短 GOP + 上一帧保持 | 14.12 dB | 12.50 dB |
| 4 slices + 短 GOP + 保持 | 16.27 dB | 15.33 dB |

组合机制能改善本组测试，但这些绝对 PSNR 仍表示明显损伤。上一帧保持减少黑屏，
不能消除已解码马赛克。不同 GOP/slice 的编码效率与实际码率也不同，完整报告包含
无丢包 PSNR、实际发包码率、缺帧率、黑帧率、重复时间戳计数与最长无新帧区间。

报告：[PDF](results/report/H265_resilience_loss_psnr_report.pdf)、
[Markdown](results/report/H265_resilience_report.md) 和
[独立 HTML](results/report/H265_resilience_report.html)（下载后用浏览器打开）。
原始逐试验 CSV、逐帧 CSV、RTP packet manifests 位于 [results/final/](results/final/)。
2% 丢包对照视频：[FourPeople](results/report/FourPeople_2percent_comparison.mp4)、
[KristenAndSara](results/report/KristenAndSara_2percent_comparison.mp4)。
原始素材、逐试验 Y4M 视频及本地构建目录不纳入仓库。

## 编译与自动化测试

需要 GStreamer >=1.26 开发文件、app/video/rtp 库和插件：x265enc、h265parse、
rtph265pay/rtph265depay、rtpjitterbuffer、avdec_h265、videoconvert、appsink/appsrc。
显示需要 autovideosink；实时写 Y4M 需要 y4menc。

在 WSL Ubuntu 中进入此目录：

```bash
cd h265-resilience  # 从仓库根目录进入
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

实测 3/3 测试通过。`live_udp` 会启动两个本地进程，占用 UDP 55464 约 5 秒，
写入 `build/live-test/`，验证真实丢包、非空输出、持续保持以及正常退出。

## 实时 UDP 收发

先启动接收端（60fps、1280x720），再启动发送端：

```bash
# 终端一：显示画面，持续 10 秒
./build/h265_live --role receiver --port 5004 --width 1280 --height 720 \
  --fps 60 --seconds 10 --latency-ms 80

# 终端二：发送 5 秒素材，4 slices、0.5 秒周期 IDR，并注入 2% RTP 丢包
./build/h265_live --role sender --input data/FourPeople.y4m --frames 300 \
  --host 127.0.0.1 --port 5004 --bitrate 3000 --slices 4 --gop 30 \
  --ip-mtu 1500 --loss .02 --seed 42
```

跨机器使用时把 `--host` 改成接收端 IP，开放接收端 UDP 端口。正常传输设置
`--loss 0`。输入暂支持固定分辨率、逐行、8bit I420 Y4M 文件。

无桌面环境时录制接收画面：

```bash
./build/h265_live --role receiver --port 5004 --width 1280 --height 720 \
  --fps 60 --seconds 10 --latency-ms 80 --headless --record received.y4m
```

`--record` 模式只写文件，不同时开预览窗口。接收端的循环按指定 fps 输出最新画面，
没有新解码帧时重复上一帧，直到 `--seconds` 到期。输出从第一张可解码图像开始；
如果参数集和启动关键帧都没收到，就没有可保持的画面。发送结束之后的保持也会
计入实时日志 `repeated_frames`，所以它不是离线表中严格源时间轴的缺帧率。

实时链路：

```
I420 -> x265enc -> h265parse -> rtph265pay -> packet loss -> udpsink
udpsrc -> rtpjitterbuffer(do-lost) -> rtph265depay -> h265parse
       -> avdec_h265 -> appsink -> fixed-cadence hold-last -> appsrc -> display/file
```

容错关键配置为 `output-corrupt=true discard-corrupted-frames=false max-errors=-1`，
depayloader 不因丢包而无限等待关键帧。`output-corrupt` 默认已经为 true，新增的
实际应用机制是显示端定时上一帧保持、分 slice 编码和周期 IDR，而不是声称实现了
新的 HEVC 解码器内部空间/运动补偿算法。周期恢复不需要反馈；本版未实现 RTCP PLI、
FEC、ARQ/RTX，也不承诺 IDR 本身丢包时必定在一个 GOP 内恢复。

## 分包口径

按 IPv4 **IP MTU 1500 bytes**：RTP 数据包最大 1472 bytes（含通常 12-byte RTP 头），
给 UDP 8 bytes 与 IP 20 bytes 留空间。单个 H.265 NAL 超过这个上限时由 rtph265pay
按 RFC 7798 分片。末包可能较短；IP 头扩展、IPv6、隧道和 Ethernet 开销未包含。

编码数据 bit flip 通常会因 UDP 校验失败表现为包丢失。本实验注入整 RTP 包丢失，
没有注入绕过校验的静默 bit flip。

## 复现完整实验

素材来源：<https://media.xiph.org/video/derf/>。FourPeople 与 KristenAndSara 标注
public domain。初始尝试原始 Y4M 下载速度过低，因此改为数据集官方 WebM 分发版本。
如果用于论文/无损原始数据对比，应下载完整原始 Y4M 后再运行，不能把当前数字直接
标为原始 Y4M 基准。

PowerShell 7 可运行 `./tools/download.ps1` 分段下载、验证每段长度并打印 SHA256。
数据准备需要系统 FFmpeg（只用于源素材解码与独立核对，实验不调用它计算结果）：

```powershell
./tools/download.ps1
ffmpeg -i data/FourPeople.webm -frames:v 300 -pix_fmt yuv420p -f yuv4mpegpipe data/FourPeople.y4m
ffmpeg -i data/KristenAndSara.webm -frames:v 300 -pix_fmt yuv420p -f yuv4mpegpipe data/KristenAndSara.y4m
```

```bash
./build/h265_resilience --data-dir data --frames 300 --out results/my-run --save-examples
./build/h265_report results/my-report results/my-run/trials.csv
```

输出目录中已有 `trials.csv` 时拒绝覆盖。结果目录使用新名字即可。
自定义素材支持 `--input clip.y4m --name NAME --frames N --out DIR`；自定义
`--losses`/`--seeds` 可用于回放工具。报告生成器面向上述标准矩阵，要求六种模式、
完整种子集合，并包含 2% 档位；自定义矩阵可直接分析 CSV。

编码先缓存为真实 RTP 包，每个包都可能丢失，包括参数集与首个 IDR。每个源时间戳
都有计分槽；缺帧补黑或保持。重复 PTS 只取第一次输出，源范围外 PTS 丢弃并另计。
不能将解码输出顺序直接与源顺序配对，也不能从分母删掉缺失帧。

PSNR-Y 对整段所有 Y 样本汇总 SSE 后计算，PSNR-YUV 对 I420 全部样本汇总
（样本数 Y:U:V=4:1:1）。三种子之间再求试验 PSNR 均值与样本标准差。
当前是离线丢包回放；没有加入抖动、RTT、迟到截止时间。真实 UDP 示例单独验证功能，
当前 PSNR 表不能解释为真实链路延迟/卡顿统计。两段 5 秒低至中运动素材不代表
高运动和长时间场景。

## 文件结构

- `src/resilience.cpp`：确定性丢包掩码、PSNR、时间轴与上一帧保持。
- `src/gst_experiment.cpp`：GStreamer 实际编码、RTP 收集与解码回放。
- `src/main.cpp`：测试矩阵、完整逐帧计分和 CSV。
- `src/live.cpp`：真实 UDP 收发与定时保持。
- `src/report.cpp`：C++ 生成可独立阅读的 HTML/SVG 和 Markdown 报告。
- `tests/`：核心、管线和 UDP 集成验证。
- `docs/verification.md`：命令、哈希与交叉验证证据。
