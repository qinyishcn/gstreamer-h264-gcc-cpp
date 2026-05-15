# H.264 + GCC Video Transmission System (C++ / GStreamer)

基于 GStreamer C API 的 H.264 视频传输系统，集成 Google Congestion Control (GCC) 拥塞控制算法。适配 Ubuntu 20.04 LTS + GStreamer 1.16+（已做跨版本兼容处理）。

## 🎯 项目简介

本项目使用纯 C/C++ 实现，基于 GStreamer 1.16 C API 构建视频传输管线：

- **H.264 编解码**：x264enc 编码 → avdec_h264 解码
- **RTP 传输**：UDP 承载 RTP 视频流
- **GCC 拥塞控制**：延迟梯度检测 + Kalman 滤波 + AIMD 速率控制
- **动态码率调整**：运行时通过 `g_object_set()` 调整 x264enc 码率

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
│   └── gcc_controller.h       # GCC 控制器头文件
├── src/
│   ├── gcc_controller.cpp     # GCC 算法实现
│   ├── sender.cpp             # GStreamer 视频发送端
│   ├── receiver.cpp           # GStreamer 视频接收端
│   └── demo.cpp               # 完整演示（含 GCC 控制）
├── CMakeLists.txt             # CMake 构建文件
├── install.sh                 # Ubuntu 20.04 安装脚本
└── README.md
```

## 🚀 快速开始

### 环境要求

- Ubuntu 20.04 LTS（推荐）/ 其他支持 GStreamer 1.16+ 的 Linux 发行版
- GStreamer 1.18+ 可获得完整的接收帧统计（fakesink `stats` 属性）；1.16 下帧统计显示为 0 但不影响传输功能
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

### 运行 Demo

```bash
# 完整演示（GCC 拥塞控制 + 模拟网络条件）
./build/demo --duration 20

# 自定义参数
./build/demo --duration 30 --bitrate 3000 --gradient-threshold 0.4
```

### 分别运行 Sender / Receiver

```bash
# 终端 1：启动接收端
./build/receiver --port 5004

# 终端 2：启动发送端
./build/sender --host 127.0.0.1 --port 5004 --bitrate 2000
```

### 关于测试视频

本项目使用 GStreamer 内置的 `videotestsrc` 元素作为视频源，**无需外部视频文件**。

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

// 获取状态
auto stats = gcc.get_stats();
printf("State: %d, Bitrate: %d kbps\n", stats.current_state, stats.current_bitrate_kbps);
```

**算法流程：**

1. `step()` 接收延迟反馈，计算延迟梯度 `gradient = delay_diff`
2. Kalman 滤波平滑梯度序列
3. 状态机判断：`|slope| < hold_thresh` → HOLD，`> grad_thresh` → DECREASE，`< -grad_thresh` → INCREASE
4. DECREASE: `bitrate *= 0.85`，INCREASE: `bitrate += 50`

### sender.cpp — GStreamer 视频发送端

```
videotestsrc → videoconvert → x264enc → h264parse → rtph264pay → udpsink
```

通过 `gst_parse_launch()` 构建管线，支持 `--bitrate`、`--preset`、`--host`、`--port` 参数。

### receiver.cpp — GStreamer 视频接收端

```
udpsrc → rtph264depay → h264parse → avdec_h264 → videoconvert → fakesink
```

实时统计已接收帧数。

### demo.cpp — 完整演示

模拟网络拥塞场景，展示 GCC 控制行为：

| 阶段 | 时段 | 网络状况 | GCC 行为 |
|------|------|----------|----------|
| Stable | 0-5s | 10ms delay, 0% loss | 保持码率 |
| Congestion | 5-10s | 10→90ms delay, 0→8% loss | 乘性降码率 |
| Recovery | 10-15s | 90→10ms delay, 8→0% loss | 加性增码率 |
| Stable | 15-20s | 10ms delay, 0% loss | 继续增长 |

运行效果：
```
  [  0.5] stable     2000 kbps  hold       Loss: 0.0%  Delay: 10.0ms  RX: 11
  [  5.5] congest    2000 kbps  hold       Loss: 0.8%  Delay: 18.2ms  RX: 161
  [  7.5] congest    1700 kbps  decrease   Loss: 4.0%  Delay: 50.2ms  RX: 241
  [ 10.0] recovery    753 kbps  decrease   Loss: 8.0%  Delay: 89.8ms  RX: 326
  [ 12.0] recovery    690 kbps  increase   Loss: 4.8%  Delay: 57.7ms  RX: 327
  [ 15.0] stable     1062 kbps  increase   Loss: 0.0%  Delay: 10.0ms  RX: 418
```

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
| `--port` | 5004 | RTP 端口 |

### Sender 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--host` | 127.0.0.1 | 目标主机 |
| `--port` | 5004 | RTP 端口 |
| `--bitrate` | 2000 | 编码码率 (kbps) |
| `--preset` | ultrafast | x264 编码预设 |
| `--duration` | 20 | 发送时长 (秒) |

## 🧪 测试验证

- ✅ GCC 控制器：稳定→拥塞→恢复 全 cycle
- ✅ GStreamer 元素：x264enc, avdec_h264, rtpbin, udpsink/src
- ✅ 集成测试：发送+接收管线，~30fps

## 📚 参考文献

- [RFC 8298] - Google Congestion Control for Real-time Web Communication
- [GStreamer 1.16 Documentation](https://gstreamer.freedesktop.org/documentation/)
- [x264enc API](https://gstreamer.freedesktop.org/documentation/x264enc.html)

## 📄 License

MIT License
