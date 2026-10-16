# 🚀 树莓派 5 + ESP32 + STM32 物联网设备控制平台

> **ESP32 无线网关**：连上 WiFi 后订阅树莓派（MQTT Broker）下发的指令，通过 UART 转发给 STM32；再把 STM32 回传的状态发布回 MQTT。**断网时数据写入外置 W25Q64 SPI Flash，断电重启后指针精准恢复、坏块自动跳过**，实现「云 — 边 — 端」三级无线控制。

[![Platform](https://img.shields.io/badge/platform-ESP32--WROOM--32-000000?logo=espressif&logoColor=white)](https://www.espressif.com/)
[![IDE](https://img.shields.io/badge/IDE-Arduino%20IDE%202.x-00979D?logo=arduino&logoColor=white)](https://www.arduino.cc/en/software)
[![Library](https://img.shields.io/badge/library-PubSubClient-2E7D32)](https://github.com/knolleary/pubsubclient)
[![Storage](https://img.shields.io/badge/storage-W25Q64%208MB%20power--loss%20cache-orange)](#-w25q64-断电记忆缓存与撕裂写入自愈)
[![Protocol](https://img.shields.io/badge/protocol-MQTT%20%7C%20UART%209600-660066)](https://mqtt.org/)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

---

## 📖 这是什么

本仓库是三级物联网控制链路中的**中间层（网关层）**固件：

- **边缘层（树莓派 5）**：MQTT Broker + 上位机，跑 Python 脚本或 Node-RED，负责界面、逻辑与数据记录。
- **网关层（ESP32）**：本仓库。连 WiFi → 订阅 MQTT 主题 → 把指令经 **UART** 转给 STM32；把 STM32 回传的数据发布回 MQTT；**网络断开时写入外置 W25Q64（8MB）**，恢复后按指针补齐。
- **终端层（STM32F103C8T6）**：硬实时执行器，控制 TB6612 电机、舵机、TCRT5000 安全互锁，回传温湿度与安全状态。

配套仓库：

| 仓库 | 角色 |
| --- | --- |
| [raspberrypi-5](https://github.com/Arduino-STM32-Dev/raspberrypi-5) | 树莓派 5 上位机 / MQTT Broker 环境搭建 |
| **esp32**（本仓库） | ESP32 无线网关：MQTT ↔ UART 双向转发 + 断网缓存 |
| [STM32](https://github.com/Arduino-STM32-Dev/STM32) | STM32F103 执行终端：边缘安全互锁与教程 |

---

## 🎬 先看效果

![ESP32 与 STM32 接线实拍](image-and-video/esp32-stm32-connect.jpg)

---

## 🏗️ 系统架构

```text
[树莓派 5 (上位机 / MQTT Broker)]  mosquitto: 0.0.0.0:1883
        ⬆ (WiFi / MQTT)  订阅 pi/to/esp32   发布 home/fan/status
[ESP32 (无线网关)]  std::queue + W25Q64 (8MB SPI Flash) 断电记忆缓存
        ⬇ (UART 9600, 8N1, RX=16 / TX=17)
[STM32F103C8T6 (执行终端)]  TB6612 电机（FAN_PWM 无级调速）/ 舵机阀门 / TCRT5000 红外互锁
```

**数据流：** `树莓派 → MQTT(pi/to/esp32) → ESP32 → SerialSTM → STM32 → 执行动作 → 回传 → ESP32 → MQTT(home/fan/status) → 树莓派`

**四个固件版本：**

| 固件 | 定位 | 特点 |
| --- | --- | --- |
| [`firmware/esp32_test`](firmware/esp32_test/esp32_test.ino) | **基础网关**（第一版） | 连接 WiFi / MQTT，指令转发 + 状态上报，逻辑最简单，适合先跑通链路 |
| [`firmware/esp32_fan_gateway`](firmware/esp32_fan_gateway/esp32_fan_gateway.ino) | **简明网关**（早期并行版本） | 与基础版同级的最小实现，配套说明见 [esp32-fan-gateway.md](esp32-fan-gateway.md) |
| [`firmware/cache_control`](firmware/cache_control/cache_control.ino) | **工业级网关**（SPIFFS 版） | 非阻塞队列、**SPIFFS 断网缓存**、恢复补发、非阻塞重连、接收缓冲区扩容 |
| [`firmware/external_cache`](firmware/external_cache/external_cache.ino) | **工业级网关**（当前版本） | 在 SPIFFS 版之上改用**外置 W25Q64 8MB SPI Flash**：元数据日志 + CRC8 **断电指针恢复**、**撕裂写入自愈**、扇区级擦写寿命优化 |

---

## 🛠️ 硬件清单

| 硬件名称 | 数量 | 说明 |
| --- | :---: | --- |
| 树莓派 5 (8GB) | 1 | 上位机 + MQTT Broker |
| ESP32 开发板 | 1 | 无线网关（本仓库固件） |
| **W25Q64 SPI Flash 模块** | 1 | **8MB（64Mbit）断电记忆缓存**，3.3V 供电，SPI 接口 |
| STM32F103C8T6 (BluePill) | 1 | 执行终端 |
| ST-Link V2 | 1 | STM32 烧录（SWD） |
| USB 转 TTL 模块 | 1 | 调试用 |
| 独立电源（电池盒 / 充电宝） | 2 | ⚠️ ESP32 与 STM32 **各自独立供电** |
| 杜邦线（母对母） | 若干 | ESP32↔STM32 只需 TX / RX / GND 三根；W25Q64 需 6 根 |

> ✅ 实测：ESP32 用**电池 + 外接板**可长期独立运行，固件烧录后无需电脑介入。

---

## 🔌 引脚接线说明

| 设备 A | 设备 B | 说明 |
| --- | --- | --- |
| ESP32 `D17` (TX2) | STM32 `PA10` (RX) | 串口发送：指令下发 |
| ESP32 `D16` (RX2) | STM32 `PA9` (TX) | 串口接收：状态回传 |
| ESP32 `GND` | STM32 `GND` | **共地（必须连接）** |

> ⚠️ **TX 接 RX、RX 接 TX**（交叉接线），且 **ESP32 与 STM32 独立供电，请勿把 VCC 直接互连**。

固件中对应的初始化（[`firmware/external_cache/external_cache.ino`](firmware/external_cache/external_cache.ino#L234-L235)）：

```cpp
SerialSTM.setRxBufferSize(1024);              // ⚠️ 必须在 begin() 之前
SerialSTM.begin(9600, SERIAL_8N1, 16, 17);    // RX=16 ← STM32 PA9(TX) / TX=17 → STM32 PA10(RX)
```

### W25Q64 外置 Flash 接线（断电记忆缓存）

| W25Q64 模块 | ESP32 | 说明 |
| --- | --- | --- |
| `VCC` | `3V3` | ⚠️ **只能 3.3V**，接 5V 会烧芯片 |
| `GND` | `GND` | 共地 |
| `CS` | **`GPIO13`** | ⚠️ **千万不要接 GPIO5 / GPIO12 / GPIO15** |
| `SCK` | `GPIO18` | VSPI SCLK |
| `MISO` | `GPIO19` | VSPI MISO |
| `MOSI` | `GPIO23` | VSPI MOSI |

> ⚠️ **血泪教训**：`CS` 最初接在 **GPIO5** 上，结果 ESP32 **完全无法下载程序**（`A fatal error occurred: Failed to connect to ESP32: No serial data received.`）。GPIO5 / GPIO12 / GPIO15 是 **Strapping 引脚**，上电电平决定启动模式，外挂 Flash 的片选把电平拉歪后芯片就进不了下载模式。改到 **GPIO13** 后一次成功，读到的 JEDEC ID 为 `0xEF 0x40 0x17`（Winbond / SPI NOR / 8MB）。细节见 [docs/02](docs/02_W25Q64_POWER_LOSS_CACHE.md#-硬件与接线)。

---

## 💻 软件依赖

### 1. 树莓派端（MQTT 服务器）

```bash
sudo apt update
sudo apt install mosquitto mosquitto-clients
# 编辑 /etc/mosquitto/mosquitto.conf，允许外部设备连接：
#   listener 1883 0.0.0.0
#   allow_anonymous true
sudo systemctl restart mosquitto
```

### 2. ESP32 端（Arduino IDE）

| 项目 | 取值 |
| --- | --- |
| 开发板 | `ESP32 Dev Module` |
| 库 | **PubSubClient**（库管理器搜索安装） |
| 外置存储 | **W25Q64**（8MB SPI Flash，裸 SPI 驱动，**不需要任何第三方库**） |
| 订阅主题 | `pi/to/esp32` |
| 发布主题 | `home/fan/status` |
| 对 STM32 串口 | `SerialSTM` = UART2，RX `16` / TX `17`，**9600 8N1**，接收缓冲区 `1024` |
| 调试串口 | `Serial` 115200 |

上传前先把 sketch 顶部的三个参数改成你自己的（出于安全考虑，仓库中为占位符）：

```cpp
const char* ssid        = "YOUR_WIFI_SSID";      // 你的 WiFi 名称（必须是 2.4GHz）
const char* password    = "YOUR_WIFI_PASSWORD";  // 你的 WiFi 密码
const char* mqtt_server = "YOUR_PI_IP";          // 树莓派的 IP 地址
```

### 3. STM32 端（Arduino IDE）

| 项目 | 取值 |
| --- | --- |
| 开发板 | `Generic STM32F1 series` → `BluePill F103C8` |
| 上传方式 | `STM32CubeProgrammer (SWD)` / STLink |
| 固件 | [`STM32/firmware/PWM-FAN`](https://github.com/Arduino-STM32-Dev/STM32/blob/main/firmware/PWM-FAN/PWM-FAN.ino)（无级调速版） |
| 核心功能 | `Serial` 收发（9600）、DHT11 温湿度、TB6612 风扇（**`FAN_PWM:XXX` 无级调速**）、舵机阀门、**TCRT5000 红外急停** |
| 上报格式 | `T:25.00,H:60.00,F:0,Spd:0,V:0,S:1`（`Spd` 为当前 PWM 速度 0~255） |
| 指令集 | `FAN_ON` / `FAN_OFF` / `VALVE_OPEN` / `VALVE_CLOSE` / **`FAN_PWM:0~255`** |

---

## 🚦 快速开始（测试指令）

在树莓派终端执行：

```bash
# 开 / 关风扇（TB6612 驱动）
mosquitto_pub -h localhost -t "pi/to/esp32" -m "FAN_ON"
mosquitto_pub -h localhost -t "pi/to/esp32" -m "FAN_OFF"

# 【新增】无级调速：0 ~ 255，数值越大转得越快
mosquitto_pub -h localhost -t "pi/to/esp32" -m "FAN_PWM:80"     # 低速、安静
mosquitto_pub -h localhost -t "pi/to/esp32" -m "FAN_PWM:200"    # 高速
mosquitto_pub -h localhost -t "pi/to/esp32" -m "FAN_PWM:0"      # 停转

# 开 / 关阀门（舵机 90° / 0°）
mosquitto_pub -h localhost -t "pi/to/esp32" -m "VALVE_OPEN"
mosquitto_pub -h localhost -t "pi/to/esp32" -m "VALVE_CLOSE"

# 订阅 ESP32 回传的状态
mosquitto_sub -h localhost -t "home/fan/status" -v
```

预期输出：

```text
Received Command from Pi: FAN_PWM:200             ← ESP32 调试串口
收到实时数据: T:30.30,H:46.00,F:1,Spd:200,V:0,S:1  ← ESP32 调试串口
home/fan/status T:30.30,H:46.00,F:1,Spd:200,V:0,S:1 ← 树莓派 mosquitto_sub
```

> ⚠️ 上报新增的 **`Spd` 字段插在 `F` 与 `V` 之间**，树莓派侧请按 **`key:value` 键名解析**，不要用固定下标，否则阀门状态会错位（详见 [STM32 docs/08](https://github.com/Arduino-STM32-Dev/STM32/blob/main/docs/08_BUG_REPORT.md)）。

> 🛡️ **安全优先**：STM32 处于红外警报（`S:0`）时，`FAN_ON` / `VALVE_OPEN` / `FAN_PWM` 会被本地互锁**拒绝执行**，而 `FAN_OFF` / `VALVE_CLOSE` 永远有效。

### 断电记忆怎么验证（W25Q64）

```text
1) 关掉树莓派（或关热点）  → 串口持续打印 [断网缓存] 写入W25Q64...
2) 观察串口出现写入日志后，直接拔掉 ESP32 电源（模拟现场断电）
3) 重新上电                → 打印 🔋 [断电恢复] 成功! WriteAddr: 0x00102A ReadAddr: 0x001000
4) 打开树莓派              → 打印 发现离线数据，开始补发... / 离线数据补发完成。
                             若断电时正在写入，还会看到 ⚠️ 检测到断电撕裂数据，跳过坏块，防止死循环...
5) 树莓派 mosquitto_sub    → 先收到补发的历史行，再收到实时行
```

> ⚠️ **烧录后必做**：`setup()` 里有一段 `🔥 正在清空历史坏数据...` 的**一次性调试代码**，它会擦掉元数据区，导致「断电记忆」每次上电都失效。看到第一次 `🆕` 输出后请立刻注释掉再重新烧录，详见 [docs/03](docs/03_BUG_REPORT.md#-p0-1每次上电都会抹掉断电记忆致命)。

---

## 📂 仓库目录结构

```text
esp32/
├── README.md                          ← 你正在读的这份文档
├── LICENSE                            ← MIT 许可证
├── .gitignore / .gitattributes
├── esp32-fan-gateway.md               ← 简明网关说明（早期并行版本）
├── firmware/
│   ├── esp32_test/                    ← 基础网关：MQTT ↔ UART 双向转发
│   │   └── esp32_test.ino
│   ├── esp32_fan_gateway/             ← 简明网关（早期并行版本）
│   │   └── esp32_fan_gateway.ino
│   ├── cache_control/                 ← 工业级网关：队列 + SPIFFS 断网缓存 + 补发
│       └── cache_control.ino
│   └── external_cache/                ← 当前版本：队列 + W25Q64 断电记忆 + 撕裂自愈
│       └── external_cache.ino
├── docs/
│   ├── 01_OFFLINE_CACHE_AND_QUEUE.md  ← 断网缓存（SPIFFS）与非阻塞队列（含三大踩坑实录）
│   ├── 02_W25Q64_POWER_LOSS_CACHE.md  ← W25Q64 断电记忆缓存与「撕裂写入」自愈
│   └── 03_BUG_REPORT.md               ← 代码审计报告 · Bug Finder（14 项缺陷清单）
└── image-and-video/
    └── esp32-stm32-connect.jpg        ← ESP32 / STM32 接线实拍
```

---

## 💾 W25Q64 断电记忆缓存与「撕裂写入」自愈

完整原理、代码拆解与排错见 **[docs/02_W25Q64_POWER_LOSS_CACHE.md](docs/02_W25Q64_POWER_LOSS_CACHE.md)**，缺陷清单见 **[docs/03_BUG_REPORT.md](docs/03_BUG_REPORT.md)**。

| 机制 | 解决什么 | 关键实现 |
| --- | --- | --- |
| **外置 W25Q64** | 内部 SPIFFS 容量小、擦写磨损快 | 裸 SPI 指令：`0x20` 擦扇区 / `0x02` 写页 / `0x03` 读数据 |
| **CS = GPIO13** | 接 GPIO5 会让 ESP32 进不了下载模式 | 避开 Strapping 引脚；先读 JEDEC ID `0xEF 0x40 0x17` 自检 |
| **扇区规划** | 元数据与数据分离，可环形复用 | Sector 0（`0x000000`）存指针，Sector 1 起（`0x001000`~`0x7FFFFF`）存数据 |
| **元数据日志 + CRC8** | 断电后精准恢复读写指针 | 16 字节记录（`0xA5` Magic + 指针 + CRC8），**写满 4KB / 256 条才擦一次** |
| **撕裂写入自愈** | 拔电导致「空记录 → 补发死循环」刷屏 | 读到空数据 → `currentReadAddr = currentWriteAddr` 强制对齐 → `break` |
| **非阻塞队列** | `client.publish()` 阻塞时串口数据被憋丢 | `dataQueue.push()` 与 `pop()+publish()` 解耦，`setRxBufferSize(1024)` 扩容 |

> 🔑 **一句话原则**：**Flash 的关键不是「写进去」，而是「擦干净再写」和「写坏了能跳过去」。**

---

## 🧠 断网缓存与非阻塞队列

完整原理、代码拆解与排错见 **[docs/01_OFFLINE_CACHE_AND_QUEUE.md](docs/01_OFFLINE_CACHE_AND_QUEUE.md)**，这里只放结论：

| 机制 | 解决什么 | 关键代码 |
| --- | --- | --- |
| **非阻塞队列** | `client.publish()` 阻塞时，串口数据被憋丢 | `dataQueue.push(receivedData)` → 之后 `pop()` + `publish()` |
| **接收缓冲区扩容** | 启动 10~15 秒内 256 字节缓冲区溢出 | `SerialSTM.setRxBufferSize(1024)`（必须在 `begin()` 前） |
| **SPIFFS 落盘** | 断网期间数据不丢，重启也不丢 | `SPIFFS.open("/offline.txt", FILE_APPEND)` + `file.println(data)` |
| **`isSyncing` 状态锁** | 补发逻辑被串口数据反复触发 → 无限补发死循环 | `if (... && !isSyncing) { isSyncing = true; ...; isSyncing = false; }` |
| **`close` → `delay(200)` → `remove`** | 句柄未释放导致 `SPIFFS.remove()` 静默失败 | 见 `loop()` 第 4 步 |
| **非阻塞重连** | 旧版 `while(!connected)` 死循环卡死主循环 | `millis()` 节流，每 5 秒试一次 |

> 🔑 **一句话原则**：**串口读取永远是最高优先级，任何可能阻塞的操作（`publish` / 读文件 / 删文件）都不能放在读串口的路径上。**

---

## 🔍 核心实现要点

| 要点 | 说明 |
| --- | --- |
| WiFi 接入 | `setup_wifi()` 阻塞等待 `WL_CONNECTED`，连上后打印 ESP32 的 IP |
| MQTT 连接 | `PubSubClient` + 随机 `clientId`，连接成功后订阅 `pi/to/esp32` |
| 断线重连 | `attemptReconnect()` 用 `millis()` 每 5 秒重试一次，**不阻塞主循环** |
| 指令下发 | `callback()` 把 payload 拼成 `String` 并 `trim()`，用 `SerialSTM.println()` 发出 |
| 串口接收 | `while (SerialSTM.available())` 逐字节拼行，遇 `'\n'` 即入队 |
| 状态上报 | 队列出队后 `client.publish("home/fan/status", data.c_str())` |
| 断网缓存 | 队列非空且离线 → 整队写入 W25Q64 数据区，并更新元数据指针 |
| 恢复补发 | 在线且读指针 ≠ 写指针 → 按地址逐条读出 `publish`，`delay(30)` 限速 |
| 断电恢复 | 上电扫描元数据区，取最后一条 CRC8 通过的记录恢复读写指针 |
| 撕裂自愈 | 读到空记录（`0xFF`）即判定为断电撕裂，指针强制对齐后跳出补发 |

> ⚠️ **换行符约定**：ESP32 用 `println()` 发送、STM32 用 `readStringUntil('\n')` 解析；STM32 回传同理。**两端必须成对使用**，否则会读到空串或粘包。

---

## 🚧 常见报错速查

| 报错 / 现象 | 原因 | 解决 |
| --- | --- | --- |
| MQTT 连接失败 `rc=-2` | mosquitto 只监听 `127.0.0.1`，或禁止匿名连接 | 修改 `/etc/mosquitto/mosquitto.conf`：`listener 1883 0.0.0.0` + `allow_anonymous true`，然后 `sudo systemctl restart mosquitto` |
| `rc=-4` | 用户名 / 密码错误 | 确认 `allow_anonymous true`，或补上 MQTT 账号密码 |
| `rc=2` / 一直重连 | `mqtt_server` 填错或树莓派不在同一网段 | `ping` 树莓派 IP；确认 ESP32 与树莓派同一 WiFi |
| WiFi 一直打印 `.` | SSID / 密码填错，或 ESP32 不支持 5GHz | 使用 **2.4GHz** 热点；核对 `ssid` / `password` 大小写 |
| 串口收到乱码 | 两端波特率或数据位不一致 | 两端统一 **9600, 8N1**（波特率改了记得同时改 STM32） |
| 上传报 `No serial data received`（连不上/进不了下载模式） | W25Q64 的 `CS` 接在了 **Strapping 引脚**（GPIO5 / GPIO12 / GPIO15），上电电平被拉歪 | `CS` 改接 **GPIO13**，先读 JEDEC ID 应为 `0xEF 0x40 0x17` |
| 串口一直刷 `🔥 正在清空历史坏数据...` + `🆕 [初始状态]` | setup() 里的**一次性调试擦除**代码没注释掉，每次上电都擦掉元数据区 | 注释掉那三行（或加 `#define FACTORY_RESET 0`），见 [docs/03](docs/03_BUG_REPORT.md) |
| W25Q64 版补发死循环 `发现离线数据，开始补发...` | 读到 `0xFF` 空记录后指针不动，`while` 条件永真 | 撕裂写入检测：`currentReadAddr = currentWriteAddr; break;` |
| 读回的数据是乱码 / 老数据被覆盖 | `Page Program` 跨 256 字节页边界会**在页内回卷**；或写入未擦除扇区 | 按页切分写入；写前判断本次是否跨扇区并先擦除 |
| 擦除后立刻写偶尔丢一条 | 只 `delay(100)`，而 4KB 扇区擦除最大需 400ms | 轮询状态寄存器 `0x05` 的 BUSY 位（bit0） |
| 日志无限刷 `发现离线数据，开始补发...`（SPIFFS 版） | 补发逻辑写在了串口循环里；或 `remove()` 因句柄未释放而失败 | 补发移到 `loop()` 顶层 + `isSyncing` 状态锁 + `file.close()` 后 `delay(200)` 再 `remove()` |
| 缓存正常，但收不到实时数据 | `publish()` 阻塞撑爆了 256 字节串口硬件缓冲区 | 用 `std::queue` 解耦；`SerialSTM.setRxBufferSize(1024)`（必须在 `begin()` 之前） |
| 联网瞬间刷出 4~5 条同时间戳数据 | 启动 10~15 秒积压，属正常现象 | 扩容缓冲区 + 队列平滑处理；想压低延迟就限制补发速率 |
| `SPIFFS Mount Failed!` | 分区表没给 SPIFFS 留空间 / 文件系统损坏 | 代码已自动 `format()` 兜底；持续失败就选 `Default 4MB with spiffs` 重新烧录 |
| `/offline.txt` 删不掉 | 文件句柄未释放 | `file.close()` 后 `delay(200)` 再 `SPIFFS.remove()` |
| STM32 收不到指令 | TX / RX 未交叉，或**没共地** | 按上表重新接线，**务必连接 GND** |
| 上传后 ESP32 反复重启 | 目标板选错 / 供电不足 | 选 `ESP32 Dev Module`；用独立电源或稳定 USB 口 |
| 树莓派收不到状态 | 订阅主题写错 | 主题核对为 `home/fan/status`，端口 `1883` |

---

## 📚 文档目录

| 篇 | 内容 | 文档 |
| :---: | --- | --- |
| **01** | 非阻塞队列 · SPIFFS 断网缓存 · 恢复补发 · 缓冲区扩容 · 三大踩坑实录 | [📄 断网缓存与非阻塞队列](docs/01_OFFLINE_CACHE_AND_QUEUE.md) |
| **02** | W25Q64 外置 Flash · Strapping 引脚翻车 · 元数据日志 + CRC8 断电恢复 · 撕裂写入自愈 | [📄 W25Q64 断电记忆缓存](docs/02_W25Q64_POWER_LOSS_CACHE.md) |
| **03** | 代码审计报告 · Bug Finder：2 个致命 + 4 个高危缺陷、复现步骤、修复补丁、自查命令 | [📄 代码审计报告](docs/03_BUG_REPORT.md) |
| — | （早期简明版）MQTT ⇄ UART 网关说明 | [📄 esp32-fan-gateway.md](esp32-fan-gateway.md) |

---

## 📈 后续计划

- [x] 基础网关跑通 MQTT ↔ UART 双向转发
- [x] 非阻塞队列 + 接收缓冲区扩容，解决网络卡顿丢包
- [x] SPIFFS 断网缓存与恢复补发（断网续传）
- [x] MQTT 非阻塞自动重连（`millis()` 节流）
- [x] **外置 W25Q64 8MB 缓存：元数据日志 + CRC8 断电指针恢复**
- [x] **撕裂写入自愈：断电坏块自动跳过，杜绝补发死循环**
- [x] 支撑 STM32 端 `FAN_PWM:0~255` 无级调速指令透传与 `Spd` 字段上报
- [ ] 修掉 [docs/03](docs/03_BUG_REPORT.md) 中的 P0/P1 缺陷（调试擦除、跨页写入、补发期 keepalive、客户端 ID 唯一化）
- [ ] 记录加「长度 + CRC8」，彻底区分半写与完整记录
- [ ] 环形缓冲容量保护（写指针追上读指针前告警并丢弃最旧数据）
- [ ] WiFi 断开也自动重连（当前只重连 MQTT）
- [ ] 补发改成分批断点续传，上报统一改为 JSON
- [ ] 树莓派端开发 Python GUI / Web 界面（或 Node-RED 流程）
- [ ] OTA 远程升级 ESP32 固件

---

## 🤝 贡献与致谢

- 欢迎提交 Issue 反馈问题，或 PR 补充你的踩坑经验。
- 本仓库的组织方式参考了 [STM32](https://github.com/Arduino-STM32-Dev/STM32) 与 [Arduino-Intro](https://github.com/Arduino-STM32-Dev/Arduino-Intro)。

## 📄 许可证

本项目采用 **MIT License**，详见 [LICENSE](LICENSE)。
