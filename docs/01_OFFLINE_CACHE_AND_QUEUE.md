# ESP32 进阶 01：断网缓存（SPIFFS）与非阻塞队列

> **本篇目标**：让网关在**网络断开时把数据存进 Flash**，网络恢复后自动补发；同时把「串口接收」和「MQTT 发送」彻底解耦，做到**不丢一条数据**
> **适用环境**：ESP32-WROOM-32 · Arduino IDE 2.x · ESP32 core 2.x/3.x · PubSubClient
> **完成标志**：拔掉树莓派 / 关掉热点 → 串口打印 `[断网缓存] 队列积压中...`；恢复网络 → 打印 `发现离线数据，开始补发...` 并把离线数据全部补齐
> ⚠️ **本篇最后会解释：为什么 `client.publish()` 会把串口数据「憋丢」—— 这是本篇最重要的一条经验**

---

## 📌 问题：网络一卡，数据就丢

早上版本的网关是这么写的（简化）：

```cpp
while (SerialSTM.available()) {
  ...
  client.publish(mqtt_topic_pub, receivedData.c_str());  // ← 阻塞点，就在读串口的循环里
}
```

STM32 每 2 秒推一行数据，看起来很轻松。但 `client.publish()` 是**同步阻塞**的（要等 TCP 把这一包写出去、要等 ACK）。一旦网络抖动，它会卡住整个 `loop()`；而 STM32 **不会因为你卡住就停下来**，它还在按 2 秒一行的节奏发。

ESP32 的 UART 硬件缓冲区默认只有 **256 字节**，几行数据就撑满了 —— 之后收到的字节**直接被硬件丢掉**，且没有任何报错。这就是「缓存功能做好了，实时数据却没了」的真相。

| 设计目标 | 手段 |
| --- | --- |
| 串口读取永不阻塞 | `std::queue<String>` 把接收与发送**解耦** |
| 断网不丢数据 | SPIFFS 文件系统落盘到 `/offline.txt` |
| 恢复后补齐 | 联网 + MQTT 就绪时按行读文件批量 `publish` |
| 补发不重复 | `isSyncing` 状态锁，保证同一时刻只有一次补发在跑 |

---

## 🛠️ 软件准备

| 项目 | 取值 |
| --- | --- |
| 开发板 | `ESP32 Dev Module` |
| 库 | **PubSubClient**（库管理器安装） |
| 文件系统 | **SPIFFS**（ESP32 core 自带 `SPIFFS.h`） |
| 订阅主题 | `pi/to/esp32` |
| 发布主题 | `home/fan/status` |
| 对 STM32 串口 | `HardwareSerial SerialSTM(2)` → RX `16` / TX `17`，**9600 8N1** |
| 调试串口 | `Serial` 115200 |

烧录前把顶部三个参数改成你自己的（仓库中为占位符）：

```cpp
const char* ssid        = "YOUR_WIFI_SSID";
const char* password    = "YOUR_WIFI_PASSWORD";
const char* mqtt_server = "YOUR_PI_IP";
```

---

## 💻 第一步：把「接收」和「发送」解耦

核心只有三行：**读串口 → `push` 进队列 → 在别处 `pop` 出来发**。

```cpp
#include <queue>

std::queue<String> dataQueue;
String receivedData = "";

void loop() {
  // ...网络处理...

  // 【2. 最优先】读取串口，只负责入队，绝对不阻塞
  while (SerialSTM.available()) {
    char c = SerialSTM.read();
    if (c == '\n') {
      if (receivedData.length() > 0) {
        dataQueue.push(receivedData);        // ← 只入队，不做网络操作
        Serial.println("收到实时数据: " + receivedData);
        receivedData = "";
      }
    } else if (c != '\r') {
      receivedData += c;                     // 按字节拼行，天然不怕分片
    }
  }

  // 【3. 之后再处理发送 / 缓存】
  if (!dataQueue.empty()) { /* 见第二步、第三步 */ }
}
```

为什么**逐字节 `read()` 拼行**，而不是 `readStringUntil('\n')`？

- `readStringUntil()` 在没等到换行符时会**一直等到超时**（默认 1000 ms），又变成一个阻塞点；
- 逐字节 `read()` 每次只取走硬件缓冲区里已有的数据，**一次 `loop()` 能清空多少就清空多少**，硬件缓冲区永远来不及溢出。

---

## 💻 第二步：把硬件接收缓冲区扩到 1024 字节

软件队列解决「来不及处理」，但**开机那 10~15 秒**（挂载文件系统 → 连 WiFi → 连 MQTT）`loop()` 一次都没跑，数据只能堆在硬件缓冲区里。默认 256 字节扛不住，所以再给它加一层保险：

```cpp
void setup() {
  Serial.begin(115200);

  SerialSTM.setRxBufferSize(1024);              // ⚠️ 必须在 begin() 之前调用
  SerialSTM.begin(9600, SERIAL_8N1, 16, 17);

  if (!SPIFFS.begin(true)) {                    // true = 挂载失败就自动格式化
    Serial.println("SPIFFS Mount Failed! Formatting...");
    SPIFFS.format();
    SPIFFS.begin(true);
  }
  Serial.println("SPIFFS Mount Success.");

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}
```

> ⚠️ `setRxBufferSize()` **必须写在 `begin()` 之前**，写在之后不生效 —— 这个顺序错误会让人误以为「扩容没用」。

---

## 💻 第三步：断网时写进 `/offline.txt`

队列里的数据只有两种命运：**网络好就发出去，网络差就落盘**。

```cpp
if (!dataQueue.empty()) {
  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    // 网络正常：从队头取一条发走
    String data = dataQueue.front();
    dataQueue.pop();
    client.publish(mqtt_topic_pub, data.c_str());
  } else {
    // 网络断开：把队里积压的全部追加写入 Flash
    Serial.println("[断网缓存] 队列积压中...");
    File file = SPIFFS.open("/offline.txt", FILE_APPEND);   // APPEND，别用覆盖模式
    if (file) {
      while (!dataQueue.empty()) {
        String data = dataQueue.front();
        dataQueue.pop();
        file.println(data);                                 // 一行一条，方便按行回读
      }
      file.close();                                         // ⚠️ 必须关，否则句柄泄漏
    }
  }
}
```

要点：

- **`FILE_APPEND`**：断网期间会写很多次，用覆盖模式会把之前的数据冲掉；
- **一次循环把队列抽干**：断网期间队列如果只写一条，会越积越多，最终耗尽内存；
- **`file.close()` 不能省**：SPIFFS 的写入需要关文件才真正落盘，也才释放句柄。

---

## 💻 第四步：恢复后补发（带状态锁）

补发逻辑**必须放在 `loop()` 顶层**，而且要用锁保证同一时刻只有一次补发在跑：

```cpp
bool isSyncing = false;

void loop() {
  // ...

  // 【4. 处理离线数据补发】只在联网 + MQTT 就绪 + 当前没有补发任务时进入
  if (WiFi.status() == WL_CONNECTED && client.connected() && !isSyncing) {
    if (SPIFFS.exists("/offline.txt")) {
      isSyncing = true;                       // ← 上锁
      Serial.println("发现离线数据，开始补发...");

      File file = SPIFFS.open("/offline.txt", FILE_READ);
      if (file) {
        while (file.available()) {
          String line = file.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
            client.publish(mqtt_topic_pub, line.c_str());
            delay(30);                        // 给 TCP 一点喘息时间，别打爆 Broker
          }
        }
        file.close();                         // ← 先关文件
        delay(200);                           // ← 等句柄真正释放
        SPIFFS.remove("/offline.txt");        // ← 再删除
        Serial.println("离线数据补发完成，文件已删除。");
      }
      isSyncing = false;                      // ← 解锁
    }
  }
}
```

三处「反直觉但必须」的写法：

| 写法 | 如果不写会怎样 |
| --- | --- |
| `!isSyncing` 状态锁 | 串口每来一行数据就触发一次补发 → **无限补发死循环** |
| 补发逻辑放在 `loop()` 顶层 | 同上，且会把读串口的主路径拖慢 |
| `file.close()` → `delay(200)` → `SPIFFS.remove()` | 句柄没释放时 `remove()` **静默失败**，文件还在，下次联网又补发一遍 |

---

## 🔧 顺带修掉的一个老毛病：非阻塞重连

旧版重连是一个 `while (!client.connected())` 死循环，**没连上就永远卡在里面**，串口数据全丢。新版改成用 `millis()` 节流，卡不住主循环：

```cpp
unsigned long lastReconnectAttempt = 0;

void attemptReconnect() {
  unsigned long now = millis();
  if (now - lastReconnectAttempt > 5000) {        // 每 5 秒才试一次，但不阻塞
    lastReconnectAttempt = now;
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      client.subscribe(mqtt_topic_sub);
    } else {
      Serial.print("failed, rc="); Serial.println(client.state());
    }
  }
}
```

---

## ✅ 验证：你应该看到什么

| 步骤 | 串口输出 |
| --- | --- |
| 上电 | `SPIFFS Mount Success.` → `WiFi connected. IP: ...` → `connected` → `Subscribed to: pi/to/esp32` |
| 正常运行（每 2 秒） | `收到实时数据: T:25.00,H:60.00,F:0,V:0,S:1` |
| 关掉热点 / 树莓派下线 | `[断网缓存] 队列积压中...`（持续打印，说明在落盘而不是丢弃） |
| 恢复网络 | `发现离线数据，开始补发...` → `离线数据补发完成，文件已删除。` |
| 树莓派侧 | `mosquitto_sub -t "home/fan/status" -v` 先收到补发的历史行，再收到实时行 |

---

## 🚧 踩坑与排错（本篇重点）

### 1. 日志无限重复 `发现离线数据，开始补发...`（死循环）

**现象**：串口疯狂刷同一句补发日志，停不下来。

**原因**：补发逻辑原本被写在**串口接收的 `while` 循环里** —— STM32 每 2 秒来一行，就触发一次补发；同时 `SPIFFS.remove()` 因为**文件句柄还没释放**而静默失败，文件一直在，于是每 2 秒「补发 → 删不掉 → 再补发」。

**解决**（两点缺一不可）：

1. 引入 `isSyncing` 状态锁，并把补发逻辑**移出串口循环、独立放到 `loop()` 顶层**；
2. `file.close()` 之后加 `delay(200)` 等句柄释放，**再**执行 `SPIFFS.remove("/offline.txt")`。

### 2. 加了缓存之后，收不到实时数据了

**现象**：断网缓存正常、恢复也能补发一次，但之后 ESP32 再也读不到 STM32 的实时数据。

**原因**：`client.publish()` 是阻塞调用，网络波动时它卡住了 `loop()`；STM32 仍在按 2 秒一行发，**256 字节的串口硬件缓冲区瞬间被撑爆**，后续字节被硬件直接丢弃。

**解决**：

1. 引入 `std::queue<String>`：串口读取只 `push`（不阻塞），在 `loop()` 后面独立 `pop` + `publish`；
2. `SerialSTM.setRxBufferSize(1024)` 扩容硬件缓冲区，抗住并发洪峰。

### 3. 开机联网成功瞬间「刷屏」

**现象**：WiFi 一连上，瞬间打印 4~5 条**时间戳几乎相同**的 STM32 数据。

**原因**：ESP32 启动要 10~15 秒（挂载 FS + 连 WiFi + 连 MQTT），这期间 `loop()` 一次都没跑，STM32 发来的数据全积压在硬件缓冲区里。`loop()` 一开跑就把积压数据一次性读出来。

**解决**：这本身就说明**没有丢数据**，属于正常现象。要让曲线更平滑，可以：

- `SerialSTM.setRxBufferSize(1024)` 扩容，避免积压期间溢出；
- 队列机制保证积压数据按序发送，不会错乱或丢失。

> ⚠️ 但要注意：**积压数据排空之前，实时数据的延迟会被拉高**。如果对实时性有要求，需要在补发时限制速率（本代码用 `delay(30)`），并优先保证串口读取始终先行。

### 4. `SPIFFS Mount Failed!` 一直出现

**原因**：分区表没给 SPIFFS 留空间，或上一次写入异常导致文件系统损坏。

**解决**：代码里已经用 `SPIFFS.begin(true)` + `SPIFFS.format()` 兜底自动格式化；若持续失败，在 Arduino IDE 里选一个**默认分区方案**（`Default 4MB with spiffs`）重新烧录。

### 5. 数据补发了，但 `home/fan/status` 上看起来「重复」

**原因**：MQTT 默认 QoS0，`publish` 只保证「尽力送达」；极端情况下补发与实时数据交错，看起来像重复。

**解决**：给每条数据加**时间戳 / 序号**前缀，由树莓派侧去重；或把发布改为 QoS1（PubSubClient 的 `publish(topic, payload, retained)` 第三参数是 retain，QoS 需改用其他 API）。

---

## 📊 数据流全景

```text
        ┌──────────── STM32 (每 2s 一行) ────────────┐
        │  T:25.00,H:60.00,F:0,V:0,S:1               │
        └──────────────────┬─────────────────────────┘
                           │ UART 9600 (RX16/TX17)
                           ▼
   ┌──────────────── ESP32 网关 ────────────────┐
   │  loop() 顺序：                             │
   │   1) 网络状态 / MQTT loop                  │
   │   2) 读串口 → push 进 std::queue（永不阻塞）│
   │   3) 队列非空：                            │
   │        ├─ 在线 → pop 一条 → publish        │
   │        └─ 离线 → 全部落盘 /offline.txt     │
   │   4) 在线且有 /offline.txt → isSyncing 补发 │
   └──────────────────┬─────────────────────────┘
                      │ MQTT  home/fan/status
                      ▼
              [树莓派 5 · Broker]
```

---

## 🔎 原理小结：为什么「队列 + 锁 + 扩容」三件套缺一不可

| 机制 | 解决什么 | 单独使用时的短板 |
| --- | --- | --- |
| `std::queue` | 把阻塞的 `publish` 移出串口读取路径 | 断电/重启后数据没了 |
| SPIFFS 落盘 | 断网、重启也不丢数据 | 补发逻辑写错就会死循环 / 重复补发 |
| `isSyncing` + `delay(200)` | 保证补发只跑一次、文件能真正删除 | 只能保证「不重复」，保证不了「不溢出」 |
| `setRxBufferSize(1024)` | 抗住启动阶段 10~15 秒的积压 | 没有队列的话，积压最终还是会在发送时堵住 |

**一句话总结：串口读取永远是最高优先级；任何可能阻塞的操作（publish / 读文件 / 删文件）都不能放在读串口的路径上。**

---

## 📈 继续往下

- [ ] 离线文件按天切分（`/offline_20261013.txt`），避免单文件无限增长
- [ ] 给缓存加**上限保护**：超过 N 条就丢最旧的，防止 SPIFFS 写满
- [ ] 补发改成分批 + 断点续传（记录已补发到的偏移量）
- [ ] WiFi 掉线也自动重连（当前只重连 MQTT）
- [ ] 用 **LittleFS** 替代 SPIFFS（ESP32 core 3.x 已默认推荐，掉电更安全）
- [ ] 指令与上报统一改成 JSON，便于树莓派侧解析

## 🔗 参考链接

- [PubSubClient](https://github.com/knolleary/pubsubclient)
- [ESP32 SPIFFS 文档](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/spiffs.html)
- 配套仓库：[STM32（执行终端 · 边缘安全互锁）](https://github.com/Arduino-STM32-Dev/STM32)
- 配套仓库：[raspberrypi-5（上位机 / Broker）](https://github.com/Arduino-STM32-Dev/raspberrypi-5)
