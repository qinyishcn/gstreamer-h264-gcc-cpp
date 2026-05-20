# H.264 + GCC Video Transmission System (C++ / GStreamer)

基于 GStreamer C API 的 H.264 视频传输系统，集成 Google Congestion Control (GCC) 拥塞控制算法。已在 GStreamer 1.16.3（Ubuntu 20.04）和 1.28.2（Ubuntu 24.04）上验证通过。

## 🎯 项目简介

本项目使用纯 C/C++ 实现，基于 GStreamer 1.16 C API 构建视频传输管线：

- **H.264 编解码**：x264enc 编码 → avdec_h264 解码
- **RTP 传输**：UDP 承载 RTP 视频流
- **GCC 拥塞控制**：延迟梯度检测 + Kalman 滤波 + AIMD 速率控制
- **动态码率调整**：运行时通过 `g_object_set()` 调整 x264enc 码率
- **反馈通道**：Receiver 通过 UDP 反馈通道向 Sender 发送丢包率和延迟梯度

### 架构

```
Sender                                    Receiver
┌──────────────┐    RTP/UDP    ┌──────────────────┐
│ videotestsrc │──────────────→│ udpsrc → depay   │
│ → x264enc    │               │ → parse → decode  │
│ → rtph264pay │               │ → convert → sink  │
│ → udpsink    │               │                   │
└──────┬───────┘               └────────┬─────────┘
       │                                │
       │    Feedback/UDP (port+1)       │
       │←───────────────────────────────│
       │  {seq, rtp_ts, rx_ts, loss%}  │
       │                                │
       ▼                                │
  GCC Controller ◄─────────────────────┘
  (adjust bitrate)
```

### 核心算法

```
Network Condition → Delay Gradient → Overuse Detector → Rate Control
                                                    ↓
                                         DECREASE (×0.85)
                                         INCREASE (+step)
                                         HOLD
```

## 📁 项目结构

```
gstreamer-h264-gcc-cpp/
├── include/
│   ├── gcc_controller.h       # GCC 控制器头文件
│   ├── feedback_channel.h     # UDP 反馈通道
│   └── gst_compat.h           # GStreamer 跨版本兼容宏
├── src/
│   ├── gcc_controller.cpp     # GCC 算法实现
│   ├── sender.cpp             # 发送端（含反馈接收）
│   ├── receiver.cpp           # 接收端（含反馈发送）
│   └── demo.cpp               # 完整演示
├── CMakeLists.txt
├── install.sh
└── README.md
```

## 🚀 快速开始

### 环境要求

- Ubuntu 20.04 LTS（推荐）/ 其他支持 GStreamer 1.16+ 的 Linux 发行版
- GStreamer 1.18+ 可获得完整的接收帧统计；1.16 下帧统计显示为 0 但不影响传输功能
- CMake 3.10+
- GCC/G++ 7+
- GStreamer 1.16.x + 开发库

### 安装依赖

```bash
# 方式一：一键安装
chmod +x install.sh && sudo ./install.sh

# 方式二：手动安装
sudo apt-get install -y \
    build-essential cmake pkg-config \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev
```

### 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行 Demo（真实反馈闭环）

```bash
# 完整演示（GCC 拥塞控制 + 真实反馈通道）
./build/demo --duration 20

# 自定义参数
./build/demo --duration 30 --bitrate 3000 --gradient-threshold 0.4
```

### 分别运行 Sender / Receiver

```bash
# 终端 1：启动接收端（自动向 sender:port+1 发送反馈）
./build/receiver --port 5004 --sender-host 127.0.0.1

# 终端 2：启动发送端（监听 port+1 接收反馈，动态调整码率）
./build/sender --host 127.0.0.1 --port 5004 --bitrate 2000
```

### 关于测试视频

本项目使用 GStreamer 内置的 `videotestsrc` 元素作为视频源，**无需外部视频文件**。

## 📖 反馈通道设计

### 协议格式

反馈通道使用轻量级二进制 UDP 包（24 字节），通过 RTP 端口 +1 传输：

| 字段 | 大小 | 说明 |
|------|------|------|
| magic | 1B | 固定值 0xCC |
| version | 1B | 协议版本（当前为 1） |
| seq | 2B | RTP 序列号 |
| sender_timestamp_us | 8B | Sender 发送时间（本地 wall clock，微秒） |
| receiver_timestamp_us | 8B | Receiver 到达时间（本地 wall clock，微秒） |
| loss_pct_x100 | 1B | 丢包率 × 100（如 35 = 0.35%） |
| padding | 3B | 保留字段 |

总大小：24 字节（`#pragma pack(1)`）

### 延迟梯度计算

1. Sender 在 RTP payload 中嵌入时间戳（90kHz 时钟）
2. Receiver 通过 pad probe 捕获每个 RTP 包：
   - 提取 RTP 序列号和时间戳
   - 记录本地到达时间（微秒精度）
3. Receiver 每 500ms 发送一个 FeedbackReport：
   - `sender_timestamp_us`：最新包的 RTP 时间戳
   - `receiver_timestamp_us`：最新包的到达时间
   - `loss_fraction`：当前丢包率
4. Sender 收到报告后：
   - `delay_ms = receiver_timestamp_us - sender_timestamp_us`（单向延迟估计）
   - GCC Controller 计算延迟梯度变化 → 状态机决策 → 调整码率

### 丢包率计算

Receiver 通过 RTP 序列号跟踪丢包：
- 维护 `received` 和 `expected` 计数器
- 序列号递增时：`expected += diff`，`received += 1`
- 丢包率 = `1 - received / expected`
- 每秒重置统计，计算瞬时丢包率

### 关键代码路径

```
Sender:
  sender.cpp → rtp_out_probe()       — 记录发出的 RTP 时间戳
             → main loop poll()       — 接收反馈，调整码率

Receiver:
  receiver.cpp → rtp_in_probe()      — 提取 RTP seq/ts，记录到达时间
               → send_report()        — 每 500ms 发送反馈

Demo:
  demo.cpp → rtp_in_probe()          — Receiver 侧 RTP 采集
           → sender_fb.poll()         — Sender 侧反馈接收
           → gcc_ctrl.step(report)    — GCC 码率调整
```

## 📖 代码说明

### gcc_controller.h / .cpp — GCC 拥塞控制

```cpp
#include "gcc_controller.h"

// 初始化控制器
gcc::Controller gcc(
    2000,    // 初始码率 kbps
    200,     // 最低码率
    8000,    // 最高码率
    0.4,     // 过载检测阈值
    0.15     // 保持阈值
);

// 处理反馈
gcc::FeedbackReport report;
report.timestamp_us = get_timestamp_us();
report.delay_ms     = measured_delay;
report.loss_fraction = loss_rate;

int new_bitrate = gcc.step(report);

// 动态调整编码器
g_object_set(G_OBJECT(encoder), "bitrate", new_bitrate, nullptr);
```

### feedback_channel.h — UDP 反馈通道

```cpp
#include "feedback_channel.h"

// Receiver 侧：发送反馈
gcc::FeedbackChannel fb("127.0.0.1", 5005, false);  // 目标 sender
fb.start();
fb.on_rtp_packet(rtp_ts_us, arrival_us, seq);  // pad probe 中调用
fb.send_report(loss_fraction);                   // 定期调用

// Sender 侧：接收反馈
gcc::FeedbackChannel fb("0.0.0.0", 5005, true);    // 监听
fb.start();
gcc::FeedbackReport report;
if (fb.poll(report)) {
    gcc.step(report);  // 喂给 GCC controller
}
```

### demo.cpp — 完整演示

Demo 在同一进程中运行 sender + receiver，通过反馈通道连接：

| 阶段 | 行为 |
|------|------|
| 启动 | Receiver 先启动，Sender 随后启动 |
| 运行 | Receiver 每 500ms 发送反馈；Sender 实时调整码率 |
| 反馈内容 | 延迟梯度 + 丢包率 → GCC Controller 决策 |

## 🔧 参数说明

### Demo 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--duration` | 20 | 演示时长 (秒) |
| `--bitrate` | 2000 | 初始码率 (kbps) |
| `--min-bitrate` | 200 | 最低码率 |
| `--max-bitrate` | 8000 | 最高码率 |
| `--gradient-threshold` | 0.4 | 过载检测阈值 |
| `--hold-threshold` | 0.15 | 保持状态阈值 |
| `--host` | 127.0.0.1 | 目标主机 |
| `--port` | 5004 | RTP 端口（反馈端口 = port+1） |

### Sender 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--host` | 127.0.0.1 | 目标主机 |
| `--port` | 5004 | RTP 端口 |
| `--bitrate` | 2000 | 初始编码码率 (kbps) |
| `--preset` | ultrafast | x264 编码预设 |
| `--duration` | 20 | 发送时长 (秒) |

### Receiver 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | 5004 | RTP 端口 |
| `--sender-host` | 127.0.0.1 | Sender IP（反馈发往此地址） |
| `--output` | fakesink | 输出方式 |

## 🧪 测试验证

- ✅ GCC 控制器：延迟梯度检测 + Kalman 滤波 + AIMD
- ✅ 反馈通道：UDP 二进制协议，24 字节轻量包
- ✅ RTP 采集：Pad probe 提取 seq + timestamp，兼容所有 GStreamer 版本
- ✅ 丢包率计算：基于 RTP 序列号间隙
- ✅ 跨版本验证：GStreamer 1.16.3（Docker/Ubuntu 20.04）+ 1.28.2（Ubuntu 24.04）

## 📚 参考文献

- [RFC 8298] - Google Congestion Control for Real-time Web Communication
- [RFC 3550] - RTP: A Transport Protocol for Real-Time Applications
- [GStreamer 1.16 Documentation](https://gstreamer.freedesktop.org/documentation/)
- [x264enc API](https://gstreamer.freedesktop.org/documentation/x264enc.html)

## 📄 License

MIT License
