# H.264 + razor GCC 实时传输

这是一个用 GStreamer C API、RTP/UDP 和 vendored razor GCC 实现的自适应
H.264 发送/接收示例。应用与新增业务代码使用 C++14；`vendor/razor` 是保留原样
集成的 C11 依赖，因此不能把整个仓库描述为纯 C++。实际拥塞控制器是
vendored razor GCC，`gcc_controller` 只保留为历史实验代码，不参与 sender、
receiver 或 demo 的控制路径。

## 架构与控制线程

发送数据路径如下：

```text
GStreamer videotestsrc -> x264enc -> rtph264pay -> appsink
    -> bounded TransportSender ingress
    -> razor callback -> C++ strict aggregate egress -> UDP
    -> successful UDP send -> RTP sequence + send history
```

`appsink` 回调只复制并排队 RTP 包。`TransportSender` 为媒体包和 probe 包分配
同一套 transport sequence；只有 UDP 成功发送后才递增 on-wire RTP sequence，
并记录真实包大小和发送时间。回调只追加原始事件；速率决策、反馈处理、probe 状态机和 razor heartbeat
都由 serialized control thread 顺序执行，避免控制器重入。

receiver 先把 RTP 到达事件放入有界 arrival queue。probe RTP 会送入 razor
接收控制器但在解码前被过滤；`MediaSequenceTranslator` 压缩共享 transport
sequence 中由 probe 造成的空洞，再把连续媒体序号交给 depay/decode。

早期实现有三个根因问题：固定按 1400 字节抽样使 ACK 码率形成错误上限；同一份
feedback 被重复消费；以及直接相减两台主机的 steady clock。当前实现逐包使用
真实 size/time、按发送记录匹配并去重反馈，并用时长差计算 RTT。

## 速率策略与 active probe

`RatePolicy` 是非对称策略：

- delay overuse 或 loss 会立即降速，单次最多下降 50%；
- Normal 状态最多按 8%/s 增长，并在显著下降后至少等待一个 RTT；
- probe 成功可快速接受实测 ACK 码率；
- 编码码率固定为 network target 的 92%；
- network target 上限为 8000 kbps，普通 pacing 上限为 9600 kbps。

active probe 使用 17..267 字节的纯 RTP padding，与媒体共享 transport
sequence；padding count 覆盖 12 字节 RTP header 后的全部字节，不携带伪媒体
payload。padding 字节预算严格按配置的 probe duration 计算，不再固定截断在
100 ms；时间回拨返回零预算，极值计算会饱和而不溢出。默认首次倍率 2.0、后续
倍率 1.5、发送 100 ms、成功阈值 0.75、失败后 5000 ms 重试、上限
8000 kbps。

C++ wire pacer 在运行中改变速率时，会按最后一个成功发送包的大小和发送时间
重算下一发送时刻：降速立即服从新上限，升速也不会保留旧速率的额外等待。

sender 与 demo 都支持：

| 参数 | 默认值 | 有效范围 |
|---|---:|---:|
| `--probe-first-scale` | 2.0 | 有限数且 > 1 |
| `--probe-next-scale` | 1.5 | 有限数且 > 1 |
| `--probe-duration-ms` | 100 | 1..299 |
| `--probe-success-ratio` | 0.75 | (0, 1] |
| `--probe-retry-ms` | 5000 | 0..600000 |
| `--probe-max-kbps` | 8000 | 0..8000，0 表示禁用 |

整数越界会安全截断；无效文本、NaN、无穷值或不满足开区间的浮点值保留默认值。
启动时只打印一次最终 `probe_config`，便于确认实际配置。
`sender --help` 与 `demo --help` 会在启动网络和 GStreamer 前打印完整范围并
成功退出；未知参数或缺值会写入 stderr 并以非零状态退出。

## 反馈协议 v4

协议 v4 使用 16 字节 network-order header，后接最多 256 字节的 razor
payload；它与旧 v3 二进制不兼容。反馈走 RTP 端口 + 1。
`loss_q8` 使用最近 256 个扩展 transport sequence 的滚动窗口；
非 probe 反馈首次高于 25/255 且 razor 没有自行降低目标时，C++ 外层兜底会
立即将目标减半；同一连续高丢包 episode 不重复兜底，回到阈值后才重新武装，
避免旧滚动窗口把目标连续减到最小值。razor 自身产生的有效 loss 降速仍会
立即执行。

| 偏移 | 字段 | 大小 | 说明 |
|---:|---|---:|---|
| 0 | magic | 1 | `0xcc` |
| 1 | version | 1 | `4` |
| 2 | sequence | 2 | 最新 transport sequence |
| 4 | loss_q8 | 1 | 丢包比例映射到 0..255 |
| 5 | flags | 1 | 保留标志 |
| 6 | payload_size | 2 | razor payload 长度，最大 256 |
| 8 | receive_time_ms | 4 | receiver 收包时本地低 32 位毫秒 |
| 12 | feedback_time_ms | 4 | receiver 发反馈时本地低 32 位毫秒 |
| 16 | payload | 0..256 | razor 反馈 |

所有多字节字段均为网络字节序。RTT 公式是：

```text
sender_elapsed = sender_feedback_time - sender_send_time
receiver_residence = receiver_feedback_time - receiver_receive_time
network_rtt = sender_elapsed - receiver_residence
```

这里只相减各自主机内部的 elapsed duration，不能直接用跨主机 steady clock
时间点做 `sender_now - receiver_rx`。

## 结构化诊断

sender 每秒以及退出前输出一行 `gcc_stats`：

- `encoder_kbps`、`target_kbps`、`pacing_kbps`：编码、网络目标和 pacer 速率；
- `acked_kbps`、`probe_delivery_kbps`、`loss_q8`、`rtt_ms`、`delay_state`：
  razor 平滑确认速率、当前 probe 的独立交付速率和拥塞状态；
- `media_sent`、`probe_sent`：实际经 pacer 发出的媒体包和 padding 包数；
- `probe_cluster_sent`：probe 窗口内实际发送的媒体加 padding 包数；
- `wire_bytes`：成功写入 UDP 的累计字节数，可用于核对实际线速；
- `validated_probe_kbps`：最近一次已通过反馈验证并接受的 probe 速率；
- `feedback`、`feedback_matched`：收到的反馈与匹配发送历史的反馈数；
- `pacer_queue_ms`、`ingress_bytes`：pacer 与 appsink ingress 积压；
- `probe_state`：`idle/probing/waiting/accepted/failed`。

receiver 每秒以及退出前输出并 flush 一行 `receiver_stats`，字段为 `frames`、
`packets`、`arrival_overflow`、`media_reorder_drops`、`last_sequence`。
loopback demo 保持 `demo_stats`，除 sender 指标外还包括 `frames`、`arrivals`、
`arrival_overflow` 与 `media_reorder_drops`。

状态真正改变时才即时输出，不在循环中重复刷屏：

```text
gcc_transition event=probe_state from=idle to=probing probe_kbps=4000
gcc_transition event=rate reason=loss from_kbps=2000 to_kbps=1200 encoder_kbps=1104
```

rate 的 `reason` 稳定取值为 `normal`、`delay_overuse`、`loss`、
`probe_accepted`。只有连续的 `normal` 增长按每秒一行限频；loss、delay
overuse 和 probe accepted 的每次实际 RatePolicy 变化都会立即输出。

## 构建与运行

需要 CMake 3.10+、支持 C++14/C11 的 GCC 或 Clang、pkg-config、pthread，
以及 GStreamer 1.16+ 的 base/good/bad/ugly/libav 插件和开发包。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

./build/demo --duration 20 --bitrate 2000
./build/demo --duration 10 --probe-first-scale 2.25 \
  --probe-duration-ms 120 --probe-success-ratio 0.8
```

分进程运行：

```bash
# 终端 1
./build/receiver --port 5004 --sender-host 127.0.0.1

# 终端 2
./build/sender --host 127.0.0.1 --port 5004 --bitrate 2000 --duration 20
```

## Sanitizer

sanitizer 仅支持非 Windows 的 GNU/Clang；在不支持的平台请求会配置失败，
避免产生“已启用”但实际未插桩的构建。ASan/UBSan 与 TSan 互斥。

```bash
cmake -S . -B build-asan -DENABLE_SANITIZERS=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DENABLE_TSAN=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-tsan -j"$(nproc)"
ctest --test-dir build-tsan --output-on-failure
./build-tsan/demo --duration 5 --bitrate 1000
```

TSan 会同时观察未由本项目插桩的系统 GLib/GStreamer 线程。在 Ubuntu 24.04 /
GStreamer 1.28 的实测报告中，竞争落在外部 `g_queue_pop_tail`、
`g_list_foreach` 和无符号的 libgstreamer 线程池/释放路径；没有项目或 razor
数据访问栈。无符号 frame 无法用最小函数级 suppression 精确覆盖，因此 TSan
配置把 `demo_runtime` 标为 `tsan_external_glib` 并禁用，只运行纯测试；
上面的手工 demo 命令仍用于如实观察平台报告。仓库不使用宽泛库级 suppression，
也不关闭 ASan leak detection。若其他平台发生 TSan shadow mapping 冲突，同样
应记录为未运行，不能宣称通过。

## Docker 与网络仿真

```bash
docker build -f Dockerfile.test -t h264-gcc-test .
docker run --rm --cap-add=NET_ADMIN h264-gcc-test
docker run --rm --cap-add NET_ADMIN h264-gcc-test bash -lc \
  'apt-get update -qq && apt-get install -qq -y iproute2 && \
   tc qdisc add dev lo root netem delay 100ms loss 10% && \
   /app/build/demo --duration 20 --bitrate 2000'
```

容器内使用 `tc netem` 时必须提供 `NET_ADMIN`。测试结束可用
`tc qdisc del dev lo root` 清理规则。

## 参考

- [razor](https://github.com/yuanrongxi/razor)
- [RFC 8298: GCC](https://www.rfc-editor.org/rfc/rfc8298)
- [RFC 3550: RTP](https://www.rfc-editor.org/rfc/rfc3550)

License: MIT。
