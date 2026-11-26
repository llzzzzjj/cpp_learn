# 基于 RK3568 的多传感器采集与远程诊断网关

这个项目在正点原子 ATK-DLRK3568 上采集板载 **SH3001 陀螺仪**和 **AP3216C 环境光**，通过 TCP 向电脑提供状态查询和实时订阅。服务端运行在嵌入式 Linux 上，使用 C++17、`poll`、`eventfd` 和 `epoll`；Ubuntu 虚拟机用于开发、交叉编译和可控回放。

已完成两路实板采集、TCP 查询、按连接订阅与取消订阅、按 `Ctrl+C` 退出，以及一轮实板低负载对照和一轮 Ubuntu 慢连接回放实验。

## 数据怎么走

```text
  SH3001 / AP3216C
          │  Linux evdev 事件
          ▼
  采集线程：poll() 等两路输入 fd 和停止 fd
          │  整理样本 → 两路各自入队 → 写传感器 eventfd
          ▼
  epoll 主线程：读取通知 → 取样本 → 更新最新值和计数
          │                         ├─ STATUS：返回当前快照
          │                         └─ SUB：有新样本时尝试推送
          ▼
  每个 TCP 连接的发送缓冲区 → EPOLLOUT / send() → 客户端
```

- **设备接入**：控制节点 `/dev/gyrosensor`、`/dev/lightsensor` 通过 `ioctl()` 启停上报。输入节点按驱动注册名 `gyro`、`lightsensor-level` 查找，不依赖 `eventN` 的编号。陀螺仪等 `SYN_REPORT` 后交付一组三轴原始值；光照从 `EV_ABS + ABS_MISC` 取得驱动上报的等级。
- **线程交接**：两路样本各有一个容量为 8 的有界队列；满时丢弃该路的最旧样本。采集线程先入队，再写传感器 `eventfd`。主线程读取通知后每轮限制两路的出队数量，给 socket 事件留出处理机会。`eventfd` 的计数表示累计通知，样本内容保存在队列里。
- **网络服务**：TCP 用换行分帧，`STATUS` 和订阅命令由主线程处理。原有 Echo 请求由线程池处理，回包走独立的结果 `eventfd`。每个连接保存自己的订阅状态和发送进度，短发送后继续发送剩余字节。
- **退出**：`Ctrl+C` 对应的 `SIGINT` 通过 `signalfd` 进入主事件循环。主线程发出独立的采集停止通知，等待采集线程结束；传感器会话退出时请求停用上报，`FdGuard` 随后关闭各 fd。

## 构建与上板

保留下面的目录关系。V1 的 CMake 目标会把采集模块和线程池一起编译，单独拷贝 `demo_test` 目录无法构建。

```text
cpp_learn/
├── common/include/fd.hpp            # 两个模块共用的 fd 管理
├── rk3568_dual_acquisition/         # 传感器采集接口、有界队列和独立测试
├── threadpool/                      # Echo 使用的线程池
└── webserver/demo_test/             # 网关程序、客户端和本 README
    └── tools/                       # 两项对照实验脚本
```

需要 Ubuntu、CMake 3.16 或更高版本、支持 C++17 的编译器；上板还需要与板端系统镜像匹配的 AArch64 工具链。下面使用正点原子工具链的默认安装路径。

在 **Ubuntu 虚拟机**里执行。首次创建 `build-atk-rk3568` 时运行带工具链参数的配置命令；已经配置过时，直接运行 `cmake --build` 即可。

```bash
cd ~/cpp_learn/webserver/demo_test

# 首次配置 RK3568 交叉构建目录。
cmake -S . -B build-atk-rk3568 \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER=/opt/atk-dlrk356x-toolchain/usr/bin/aarch64-buildroot-linux-gnu-g++

# 构建并确认产物类型；预期看到 ARM aarch64。
cmake --build build-atk-rk3568 --target epoll_server
file build-atk-rk3568/epoll_server

# IP 换成板子的当前 IPv4 地址。
BOARD_IP=192.168.137.45
scp build-atk-rk3568/epoll_server "root@${BOARD_IP}:/data/lzj/cpp_learn/epoll_server_gateway"
```

在 **RK3568** 的 SSH 终端执行。先确认旧服务没有占用 TCP 8888 端口：

```bash
cd /data/lzj/cpp_learn
# 启动真实的板载传感器采集。
./epoll_server_gateway --sensors
```

在 **Ubuntu** 上编译客户端，并在两个独立终端各启动一个。程序会提示输入服务端 IPv4 地址，端口固定为 `8888`。

```bash
cd ~/cpp_learn/webserver/demo_test
cmake -S . -B build-native
cmake --build build-native --target client_test
./build-native/client_test
```

如果只在 Ubuntu 验证网络链路，构建 `epoll_server` 本机版，用 `./build-native/epoll_server` 启动，不加 `--sensors`，客户端填 `127.0.0.1`。此时 `STATUS` 的样本数为 0、读数显示 `NA`，Echo 仍可使用。

## 命令与演示

每条命令输入后按回车。命令区分大小写；`STATUS`、`SUB` 和 `UNSUB` 接受 LF 或 CRLF 行尾。命令回复和主动推送都各占一行。

| 命令 | 回复或行为 |
|---|---|
| `STATUS` | 例如 `STATUS gyro_count=145 gyro_raw=-9,-133,24 light_count=3 light_level=0`。计数表示主线程已消费的样本数；三轴是原始值，光照是等级而非 `lux`；尚无读数时显示 `NA`。 |
| `SUB gyro` | `OK SUB gyro`；后续可能收到 `DATA gyro x=-9 y=-133 z=24`。按样本时间至少间隔 100 ms 才尝试推送最新值。 |
| `SUB light` | `OK SUB light`；后续可能收到 `DATA light level=0`。等级 `0` 是有效值。 |
| `UNSUB` | `OK UNSUB`；取消当前连接的两路订阅，不影响其他连接。一个连接可同时订阅两路。 |
| `hello` | `HELLO`，保留原始 Echo 服务。 |

1. A 发送 `SUB gyro`，B 发送 `SUB light`。轻转开发板，再遮住并移开光照传感器，确认两边各收到订阅的数据。
2. 两边分别发送 `STATUS`，确认都能查询两路快照；光照没有新事件时，光照计数可以暂时不变。
3. A 发送 `UNSUB`，确认 A 停止收到新推送，B 的光照订阅仍有效。A 发 `hello` 还能收到 `HELLO`。
4. 先退出客户端，保持服务端运行；服务端会按连接打印 `[tx]` 汇总。最后在板端按 `Ctrl+C`，检查能正常退出并重新启动。

## 慢客户端实验

两轮中，B 始终持续读取，并定期发 `STATUS`。第一轮 A 也持续读取；第二轮 A 收到订阅确认后保持 TCP 连接打开，但停止调用 `recv()`。`Send-Q` 在连接打开时从服务端观察；`[tx]` 则在客户端断开后输出。

### RK3568 实板低负载基线

在电脑上用 Python 运行脚本，对板上运行的服务做两轮各 30 秒的陀螺仪订阅测试：

```bash
# 两轮分别由脚本让 A 正常读、停读，B 自动接收和查询。
BOARD_IP=192.168.137.45  # 换成板子的当前地址。
python3 tools/rk3568_slow_client_check.py "$BOARD_IP" --seconds 30
```

如果板上有 `ss`，可在 A 仍连接时运行 `ss -tin 'sport = :8888'`，按脚本打印的 A 本地端口匹配服务端的 `Peer Address:Port`，重复观察 `Send-Q`。

| 30 秒对照 | A `skip_pending` | B 收到的推送 | B 的 `STATUS` |
|---|---:|---:|---:|
| A 正常读取 | 0 | 239 条 | 5/5，无超时 |
| A 停止读取 | 0 | 242 条 | 5/5，无超时 |

四个连接均未记录到短发送或 `would_block`，这次没有采到板端 `Send-Q`。结果仅说明**当前实板流量下，A 停读 30 秒时 B 仍可正常接收和查询**；没有确认 A 的 TCP 发送缓冲区发生持续积压。

### Ubuntu 可控回放：复现积压

在 **Ubuntu** 运行下面的命令，需要 Python 3.8+、CMake、本机 C++ 编译器和 `ss`。脚本从 `cpp_learn` 复制源码至临时目录，只监听本机回环地址，使用每 2 ms 一条的模拟光照数据。两轮均把服务端 `SO_SNDBUF` 和 A 的 `SO_RCVBUF` 请求值设为 4,096 字节，以便更快触发积压；不会改动项目源文件或板上的服务。

```bash
# 在 V1 工程目录运行；每轮 9 秒。
python3 tools/rk3568_gateway_replay_test.py --repo "$HOME/cpp_learn" --seconds 9
```

| 9 秒对照 | A 的 `Send-Q` | A `skip_pending` | B 收到的推送 | B 的 `STATUS` |
|---|---|---:|---:|---:|
| A 正常读取 | 大部分时间为 0，最高 19 字节 | 26 次 | 4,449 条 | 9/9，无超时 |
| A 停止读取 | 升到 1,136 字节后持续积压 | 4,114 次 | 4,473 条 | 9/9，无超时 |

在这组回放条件下，A 的 TCP 发送积压和应用侧推送跳过都明显增加，B 仍能接收数据并完成所有查询。`would_block` 可以是 `0`：socket 暂时不可写时，epoll 可能不通知程序调用 `send()`。这组结果证明了**受控回放条件下的慢连接隔离**；不能据此推断 RK3568 真实传感器的极限吞吐、延迟或丢样率。

## 当前取舍

- **尽力推送实时读数**：每个连接只保留一条待发送的消息。正在等待 Echo 或上一条消息未发完时，跳过新推送，不覆盖半条已发出的 TCP 消息，也不补发历史样本。
- **分层看丢弃**：采集队列满时丢掉最旧的样本；`skip_pending` 记录的是某个订阅连接因仍有待发数据而跳过的新推送。两者都不同于传感器芯片内部的丢样。
- **部署范围**：用于可信局域网内的学习与演示。当前版本没有认证、加密或历史存储；本项目验证的是采集、通信和连接隔离。
