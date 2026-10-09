# H.264 + GCC Video Transmission System (C++ / GStreamer)

基于 GStreamer C API 的 H.264 视频传输系统，集成 [razor](https://github.com/yuanrongxi/razor) GCC 拥塞控制算法。已在 GStreamer 1.16.3（Ubuntu 20.04）和 1.28.2（Ubuntu 24.04）上验证通过。

## H.265 丢包恢复实验（独立 C++17 子项目）

[h265-resilience](h265-resilience/README.md) 提供 H.265 UDP 收发、容错解码与上一帧保持、
多 slice/短 GOP 周期恢复，以及两段 720p 素材、六种机制、七档丢包率的 252 组实测。
查看 [PDF 报告](h265-resilience/results/report/H265_resilience_loss_psnr_report.pdf)、
[结果说明](h265-resilience/results/report/H265_resilience_report.md) 和
[逐帧/逐试验 CSV](h265-resilience/results/final/)。本子项目未接入 GCC，不改变原 H.264 行为。

## 🎯 项目简介

本项目使用纯 C/C++ 实现，基于 GStreamer C API 构建视频传输管线：

- **H.264 编解码**：x264enc 编码 → avdec_h264 解码
- **RTP 传输**：UDP 承载 RTP 视频流
- **razor GCC 拥塞控制**：延迟梯度检测 + Kalman 滤波 + AIMD 速率控制（via razor library）
- **Loss-based 自适应**：丢包率平滑 + 20% max step 限幅
- **RTT 估算**：反馈通道携带接收/发送时间戳，sender 端计算真实 RTT
- **动态码率调整**：运行时通过 `g_object_set()` 调整 x264enc 码率

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
       │  {seq, loss%, rx_ts, fb_ts,    │
       │   razor_payload}               │
       ▼                                │
  razor::Sender ◄──────────────────────┘
  (GCC bandwidth estimation)
  + loss-based adaptation
  (adjust x264enc bitrate)
```

## 📁 项目结构

```
gstreamer-h264-gcc-cpp/
├── include/
│   ├── gcc_controller.h       # 自研 GCC 控制器（备用）
│   ├── feedback_channel.h     # UDP 反馈通道 + LossTracker
│   ├── razor_wrapper.h        # razor C++ 封装（kbps↔bps 转换）
│   └── gst_compat.h           # GStreamer 跨版本兼容宏
├── src/
│   ├── gcc_controller.cpp     # 自研 GCC 算法实现
│   ├── sender.cpp             # 发送端（razor + loss-based 适配）
│   ├── receiver.cpp           # 接收端（razor 反馈 + LossTracker）
│   └── demo.cpp               # 完整演示（同进程 sender+receiver）
├── vendor/razor/              # razor 库源码（vendored）
├── CMakeLists.txt
├── Dockerfile.test            # GStreamer 1.16 Docker 测试环境
├── docker-test.sh             # 自动化测试脚本
├── install.sh
└── README.md
```

## 🚀 快速开始

### 环境要求

- Ubuntu 20.04 LTS（推荐）/ 其他支持 GStreamer 1.16+ 的 Linux 发行版
- CMake 3.10+
- GCC/G++ 7+（支持 C++14）
- GStreamer 1.16.x + 开发库
- pthread

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
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 运行 Demo

```bash
# 完整演示（razor GCC + loss-based 自适应 + 真实反馈通道）
./build/demo --duration 20

# 自定义参数
./build/demo --duration 30 --bitrate 3000
```

### 分别运行 Sender / Receiver

```bash
# 终端 1：启动接收端
./build/receiver --port 5004 --sender-host 127.0.0.1

# 终端 2：启动发送端
./build/sender --host 127.0.0.1 --port 5004 --bitrate 2000
```

### Docker 测试（GStreamer 1.16）

```bash
# 构建测试镜像（Ubuntu 20.04 = GStreamer 1.16.3）
docker build -f Dockerfile.test -t h264-gcc-test .

# 运行基础测试
docker run --rm h264-gcc-test

# 运行带 tc/netem 的网络条件测试
docker run --rm --cap-add NET_ADMIN h264-gcc-test bash -c \
    "apt-get update -qq && apt-get install -qq -y iproute2 && bash /app/docker-test.sh"
```

## 📖 反馈通道设计

### 协议格式（v3）

反馈通道使用轻量级二进制 UDP 包（18 字节头 + 变长 payload），通过 RTP 端口 +1 传输：

| 字段 | 大小 | 说明 |
|------|------|------|
| magic | 1B | 固定值 0xCC |
| version | 1B | 协议版本（3 = razor + RTT） |
| seq | 2B | 最新 RTP 序列号 |
| loss_pct_x100 | 1B | 丢包率 × 100（如 5 = 0.05%） |
| fb_size | 2B | razor 反馈数据大小 |
| rx_ts_ms | 4B | Receiver 收到包的时间（ms） |
| fb_ts_ms | 4B | Receiver 发送反馈的时间（ms） |
| padding | 2B | 保留字段 |
| fb_data | 0-256B | razor 反馈 payload |

### RTT 估算

Sender 收到反馈后计算：
```
RTT = now_ms - fb.rx_ts_ms
```
其中 `now_ms` 是 sender 收到反馈的本地时间，`rx_ts_ms` 是 receiver 收到 RTP 包的时间。这个值包含了 receiver 处理延迟 + 网络传输延迟，乘以约 2 倍即为近似 RTT。

### 丢包率计算

Receiver 通过 RTP 序列号跟踪丢包（`LossTracker` 类）：
- 维护 `received` 和 `expected` 计数器
- 处理 16-bit 序列号环绕
- 丢包率 = `1 - received / expected`

### 关键代码路径

```
Sender:
  sender.cpp → rtp_out_probe()          — 记录发出的 RTP seq
             → main loop poll()          — 接收反馈
             → update_rtt()              — 更新 razor RTT
             → on_feedback()             — 喂给 razor 做带宽估计
             → loss-based callback       — 丢包率平滑 + 码率调整

Receiver:
  receiver.cpp → rtp_in_probe()         — 提取 RTP seq，记录到达时间
               → razor_rx.on_received()  — 通知 razor 收到包
               → razor_rx.heartbeat()    — razor 生成反馈
               → send_feedback()         — 发送反馈（含时间戳）

Demo:
  demo.cpp → 同进程 sender + receiver
           → 通过 FeedbackChannel 连接
           → razor_tx + razor_rx 双向拥塞控制
```

## 🔧 参数说明

### Demo 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--duration` | 20 | 演示时长 (秒) |
| `--bitrate` | 2000 | 初始码率 (kbps) |
| `--port` | 5004 | RTP 端口（反馈端口 = port+1） |

### Sender 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--host` | 127.0.0.1 | 目标主机 |
| `--port` | 5004 | RTP 端口 |
| `--bitrate` | 2000 | 初始编码码率 (kbps) |
| `--preset` | ultrafast | x264 编码预设 |
| `--duration` | 20 | 发送时长 (秒) |
| `--threads` | 2 | x264 编码线程数 |

### Receiver 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | 5004 | RTP 端口 |
| `--sender-host` | 127.0.0.1 | Sender IP（反馈发往此地址） |

## 🧪 测试验证（GStreamer 1.16.3 Docker）

| 场景 | RTT | 码率行为 | 反馈数 |
|------|-----|---------|--------|
| Baseline (no tc) | 20-40ms | 2000→2080 kbps ↑ | ~480 |
| 50ms delay + 5% loss | 50-70ms | 2000→1850-2000 kbps | ~350 |
| 100ms delay + 10% loss | 85-140ms | 2000→1600-1800 kbps | ~490 |
| 200ms delay + 15% loss | 100-150ms | 2000→1300-1500 kbps | ~430 |

### 已知限制

- GStreamer 1.16 下 receiver 的解码帧统计显示为 0（不影响传输功能）
- 高丢包+高延迟下解码器可能暂时卡住（pipeline 本身正常）
- RTT 估算为近似值（包含 receiver 处理延迟）

## 📚 参考文献

- [razor](https://github.com/yuanrongxi/razor) - GCC/BBR/REMB 拥塞控制库
- [RFC 8298] - Google Congestion Control for Real-time Web Communication
- [RFC 3550] - RTP: A Transport Protocol for Real-Time Applications
- [GStreamer 1.16 Documentation](https://gstreamer.freedesktop.org/documentation/)

## 📄 License

MIT License
