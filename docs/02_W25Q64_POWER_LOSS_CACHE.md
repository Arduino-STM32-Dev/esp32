# ESP32 进阶 02：外置 W25Q64 断电记忆缓存与「撕裂写入」自愈

> **本篇目标**：把断网缓存从「内部 SPIFFS」升级到**外置 W25Q64 SPI Flash（8MB）**，做到**断电重启后指针不丢、数据继续补发、坏块自动跳过**
> **适用环境**：ESP32-WROOM-32 · Arduino IDE 2.x · ESP32 core 2.x/3.x · PubSubClient · W25Q64 模块（3.3V）
> **完成标志**：断网写入 → 打印 `[断网缓存] 写入W25Q64...`；**直接拔掉 ESP32 电源再上电** → 打印 `🔋 [断电恢复] 成功! WriteAddr: 0x00102A ReadAddr: 0x001000`，随后自动补发并打印 `离线数据补发完成。`
> ⚠️ 本篇最后有一段**必须删掉的调试代码**（`🔥 正在清空历史坏数据...`），忘了删就会发现「断电记忆永远失效」——见 [docs/03_BUG_REPORT.md](03_BUG_REPORT.md) 第 1 条。

---

## 📌 为什么内部 SPIFFS 不够用

[docs/01](01_OFFLINE_CACHE_AND_QUEUE.md) 里用 SPIFFS 解决了「断网落盘」，但实测暴露了三个硬伤：

| 问题 | 说明 |
| --- | --- |
| 容量小 | 内部 Flash 要分给程序（1.2MB+）、OTA、NVS，能留给缓存的分区通常只有 **1~1.5MB** |
| 磨损快 | 断网时整队 `FILE_APPEND` 反复重写文件，同一块扇区被反复擦写，擦写寿命（约 10 万次）很快见底 |
| **撕裂写入** | 掉电瞬间若正在写元数据，会出现「数据写了一半、指针却更新了」，重启后读指针指向 `0xFF` 空区 → **`发现离线数据，开始补发...` 死循环刷屏** |

外置 W25Q64 一次到位：**8MB 容量 + 独立芯片（写坏了换芯片，不伤主控）+ 可以自己规划扇区做环形缓冲**。

---

## 🛠️ 硬件与接线

| W25Q64 模块 | ESP32 | 说明 |
| --- | --- | --- |
| `VCC` | `3V3` | ⚠️ **只能 3.3V**，5V 会烧芯片 |
| `GND` | `GND` | 共地 |
| `CS` | **`GPIO13`** | ⚠️ **千万不要用 GPIO5 / GPIO15 / GPIO12** |
| `SCK` | `GPIO18` | VSPI 默认 SCLK |
| `MISO` | `GPIO19` | VSPI 默认 MISO |
| `MOSI` | `GPIO23` | VSPI 默认 MOSI |

### ⚠️ 为什么 CS 必须是 GPIO13：一次真实的「变砖」

最初把 `CS` 接在 **GPIO5**，结果 **ESP32 完全无法下载程序**，Arduino IDE 一直报：

```text
A fatal error occurred: Failed to connect to ESP32: No serial data received.
```

原因：**GPIO5 / GPIO15 / GPIO12 是 ESP32 的 Strapping 引脚**，上电瞬间的电平决定芯片启动模式（是否进入下载模式、Flash 电压选择等）。W25Q64 的 `CS` 在空闲时被外部电路拉低，正好把 GPIO5 的启动电平拽歪，芯片压根进不了下载模式。

> **经验**：外挂 SPI 设备的 `CS` 必须避开 **GPIO5 / GPIO12 / GPIO15**（Strapping），也不要碰 GPIO6~11（内部 Flash 专用，一碰必崩）。GPIO13 是安全选择。
> 改线后先跑一次 JEDEC ID 自检，确认芯片通信正常再往下写：

```cpp
// 读 JEDEC ID：发送 0x9F，应返回 0xEF 0x40 0x17（Winbond / 64Mbit）
digitalWrite(FLASH_CS, LOW);
SPI.transfer(0x9F);
uint8_t m1 = SPI.transfer(0x00), m2 = SPI.transfer(0x00), m3 = SPI.transfer(0x00);
digitalWrite(FLASH_CS, HIGH);
Serial.printf("JEDEC ID: 0x%02X 0x%02X 0x%02X\n", m1, m2, m3);   // → 0xEF 0x40 0x17
```

| 返回字节 | 含义 |
| --- | --- |
| `0xEF` | 厂商 = Winbond |
| `0x40` | 存储类型 = SPI NOR |
| `0x17` | 容量 = 2^23 = **8MB（64Mbit）** |

---

## 💻 第一步：抛弃现成库，用底层 SPI 指令驱动

试过的几个 W25Q64 库要么与 ESP32 core 3.x 冲突，要么只能整块读写、不支持我们要的「按扇区擦 + 按地址写」。最后**直接用 SPI 协议裸奔**，三个指令搞定：

| 指令 | 代码 | 作用 |
| --- | --- | --- |
| `Sector Erase` | `0x20` | 擦除 4KB 扇区，**擦完变 0xFF**（Flash 只能 1→0，写之前必须先擦） |
| `Page Program` | `0x02` | 写入最多 256 字节（**不能跨页**） |
| `Read Data` | `0x03` | 从任意地址连续读 |

```cpp
#define FLASH_CS   13
#define FLASH_SCK  18
#define FLASH_MISO 19
#define FLASH_MOSI 23

void flashEraseSector(uint32_t addr) {
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x20);                       // Sector Erase (4KB)
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer( addr        & 0xFF);
  digitalWrite(FLASH_CS, HIGH);
  delay(100);                               // 等擦除完成
}

void flashWriteString(uint32_t addr, String data) {
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x02);                       // Page Program
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer( addr        & 0xFF);
  for (uint16_t i = 0; i < data.length(); i++) SPI.transfer(data[i]);
  digitalWrite(FLASH_CS, HIGH);
  delay(5);
}

String flashReadString(uint32_t addr) {
  String data = "";
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x03);                       // Read Data
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8)  & 0xFF);
  SPI.transfer( addr        & 0xFF);
  for (uint16_t i = 0; i < 256; i++) {
    char c = SPI.transfer(0x00);
    if (c == '\n' || c == 0xFF) break;      // 0xFF = 未写过 → 记录结束
    data += c;
  }
  digitalWrite(FLASH_CS, HIGH);
  return data;
}
```

初始化（注意 CS 要先拉高，且在 `SPI.begin()` 里声明引脚）：

```cpp
pinMode(FLASH_CS, OUTPUT);
digitalWrite(FLASH_CS, HIGH);
SPI.begin(FLASH_SCK, FLASH_MISO, FLASH_MOSI, FLASH_CS);
SPI.setFrequency(1000000);                  // 1MHz，杜邦线长时更稳
```

> **为什么用 `0xFF` 当记录结束符**：擦除后的 Flash 全是 `0xFF`，而我们缓存的是 ASCII 文本行。读到 `0xFF` 就说明这条记录后面是空白区，天然成为分隔符——**代价是必须接受「记录写入时多留 1 字节空洞」**（代码里 `currentWriteAddr += data.length() + 1`）。

---

## 💻 第二步：扇区规划（8MB 怎么分）

```cpp
#define META_SECTOR_START 0x000000    // 元数据区：Sector 0 (4KB)
#define DATA_SECTOR_START 0x001000    // 数据区起点 = Sector 1
#define FLASH_MAX_ADDR    0x7FFFFF    // 8MB 最大地址（0x000000~0x7FFFFF）
#define FLASH_SECTOR_SIZE 4096        // 一次擦 4KB

uint32_t currentWriteAddr = DATA_SECTOR_START;   // 写指针
uint32_t currentReadAddr  = DATA_SECTOR_START;   // 读（补发）指针
```

```text
0x000000 ┌──────────────────┐
         │ 元数据区 Sector 0 │  16 字节/条 指针日志，最多 256 条
0x001000 ├──────────────────┤
         │                  │
         │  数据环形缓冲区   │  ≈ 8MB - 4KB，写满回卷到 0x001000
         │                  │
0x7FFFFF └──────────────────┘
```

- 读指针 = 已补发到哪；写指针 = 已缓存到哪。**两者相等 = 没有待补发数据**。
- 写指针超过 `FLASH_MAX_ADDR` 就回卷到 `DATA_SECTOR_START`（环形复用）。

---

## 💻 第三步：元数据日志 + CRC8 → 断电恢复指针

**只存一条指针是不行的**：断网缓存时每写一条数据就要更新一次指针，如果每次都擦写 Sector 0，4KB 扇区几秒就被磨穿。所以改成 **日志追加式**：一条记录 16 字节，往后追加，**写满 256 条才擦一次扇区**，磨损直接降低两个数量级。

```cpp
void savePointers() {
  uint8_t record[16];
  memset(record, 0xFF, 16);

  static uint8_t seq = 0;
  seq++;

  record[0]  = 0xA5;                              // Magic
  record[1]  = seq;                               // 序号
  record[2]  = (currentWriteAddr >> 24) & 0xFF;   // 写指针 4 字节
  record[3]  = (currentWriteAddr >> 16) & 0xFF;
  record[4]  = (currentWriteAddr >> 8)  & 0xFF;
  record[5]  =  currentWriteAddr        & 0xFF;
  record[6]  = (currentReadAddr  >> 24) & 0xFF;   // 读指针 4 字节
  record[7]  = (currentReadAddr  >> 16) & 0xFF;
  record[8]  = (currentReadAddr  >> 8)  & 0xFF;
  record[9]  =  currentReadAddr         & 0xFF;
  record[10] = crc8(record, 10);                  // 前 10 字节的 CRC8

  // 从元数据区起点往后扫，找到第一个 0xFF（未写过）的位置
  uint32_t metaAddr = META_SECTOR_START;
  while (metaAddr < META_SECTOR_START + FLASH_SECTOR_SIZE) {
    /* 读 1 字节 magic */
    if (magic == 0xFF) break;
    metaAddr += 16;
  }
  if (metaAddr >= META_SECTOR_START + FLASH_SECTOR_SIZE) {   // 写满了
    flashEraseSector(META_SECTOR_START);
    metaAddr = META_SECTOR_START;
  }
  /* 在 metaAddr 处 Page Program 这 16 字节 */
}
```

`crc8` 用标准多项式 `0x8C`（CRC-8/Dallas/Maxim，和 DS18B20 同一套）：

```cpp
uint8_t crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    uint8_t extract = *data++;
    for (uint8_t i = 8; i; i--) {
      uint8_t sum = (crc ^ extract) & 0x01;
      crc >>= 1;
      if (sum) crc ^= 0x8C;
      extract >>= 1;
    }
  }
  return crc;
}
```

上电恢复：**扫完整个元数据区，取最后一条 CRC 校验通过的记录**（后面的都是 `0xFF`，被 Magic 判断跳过）：

```cpp
void loadPointers() {
  uint32_t lastValidWrite = DATA_SECTOR_START, lastValidRead = DATA_SECTOR_START;
  bool found = false;

  for (uint32_t addr = META_SECTOR_START; addr < META_SECTOR_START + FLASH_SECTOR_SIZE; addr += 16) {
    uint8_t record[16];
    /* 连续读 16 字节 */
    if (record[0] == 0xA5 && record[10] == crc8(record, 10)) {
      lastValidWrite = (record[2] << 24) | (record[3] << 16) | (record[4] << 8) | record[5];
      lastValidRead  = (record[6] << 24) | (record[7] << 16) | (record[8] << 8) | record[9];
      found = true;
    }
  }
  if (found) {
    currentWriteAddr = lastValidWrite;
    currentReadAddr  = lastValidRead;
    Serial.print("🔋 [断电恢复] 成功! WriteAddr: 0x"); Serial.print(currentWriteAddr, HEX);
    Serial.print(" ReadAddr: 0x"); Serial.println(currentReadAddr, HEX);
  } else {
    currentWriteAddr = currentReadAddr = DATA_SECTOR_START;
    Serial.println("🆕 [初始状态] 未发现历史指针，从数据区起点开始。");
  }
}
```

| 设计点 | 作用 |
| --- | --- |
| `0xA5` Magic | 快速跳过空白区（`0xFF`）和垃圾数据 |
| `seq` 序号 | 预留的记录版本号 |
| **CRC8** | **一条记录被写坏（撕裂）时校验失败，直接跳过**，指针不会恢复到垃圾值 |
| 日志追加 | 写满 4KB 才擦一次 → 擦写寿命从「每次写都擦」变成「每 256 次写才擦」 |

---

## 💻 第四步：「撕裂写入」自愈（本篇最关键的 8 行代码）

### 问题怎么复现的

正常补发时，`currentReadAddr` 追着 `currentWriteAddr` 走。但**如果在补发过程中拔掉电源**，可能出现：

1. 数据区的某条记录只写了一半（后半是 `0xFF`）；
2. 元数据里**指针已经更新**，指向这个坏记录。

重启后：读指针指向一条**空记录**（首字节就是 `0xFF`）→ `flashReadString()` 返回空串 → 代码进 else 分支却**没有移动指针**，`while (currentReadAddr != currentWriteAddr)` 永远为真 →

```text
发现离线数据，开始补发...
发现离线数据，开始补发...
发现离线数据，开始补发...          ← 串口刷屏，系统卡死
```

### 修复：读到空数据 = 判定撕裂，强制对齐指针

```cpp
while (currentReadAddr != currentWriteAddr) {
  if (currentReadAddr > FLASH_MAX_ADDR) currentReadAddr = DATA_SECTOR_START;

  String line = flashReadString(currentReadAddr);
  if (line.length() > 0) {
    client.publish(mqtt_topic_pub, line.c_str());
    currentReadAddr += line.length() + 1;
    savePointers();
    delay(30);
  } else {
    // 【关键修复】读到空数据（0xFF）→ 拔电时这块写坏了
    Serial.println("⚠️ 检测到断电撕裂数据，跳过坏块，防止死循环...");
    currentReadAddr = currentWriteAddr;   // 强制对齐，丢弃坏记录
    savePointers();
    break;                                // 跳出补发循环
  }
}
```

**为什么敢直接跳到写指针**：这条坏记录之后的数据要么是「还没写」（`0xFF`，本来就是空的），要么是「被断电截断的」，都无法保证完整性。宁可丢这一小段，也不能让设备卡死在死循环里 —— **工业设备的底线是「自愈」，不是「零丢失」**。

---

## 💻 第五步：队列照旧，写入换成 Flash

非阻塞队列（`std::queue<String>`）的设计完全沿用 [docs/01](01_OFFLINE_CACHE_AND_QUEUE.md)，只把落盘动作换成 Flash 写入：

```cpp
if (!dataQueue.empty()) {
  if (WiFi.status() == WL_CONNECTED && client.connected()) {
    String data = dataQueue.front(); dataQueue.pop();
    client.publish(mqtt_topic_pub, data.c_str());
  } else {
    Serial.println("[断网缓存] 写入W25Q64...");
    while (!dataQueue.empty()) {
      String data = dataQueue.front(); dataQueue.pop();

      if (currentWriteAddr % FLASH_SECTOR_SIZE == 0) {   // 进入新扇区先擦除
        flashEraseSector(currentWriteAddr);
      }
      flashWriteString(currentWriteAddr, data);
      currentWriteAddr += data.length() + 1;

      if (currentWriteAddr > FLASH_MAX_ADDR) currentWriteAddr = DATA_SECTOR_START;
      savePointers();                                    // 每写一条记录更新一次指针
    }
  }
}
```

> ⚠️ `if (currentWriteAddr % FLASH_SECTOR_SIZE == 0)` 这个「对齐才擦」的判据**只覆盖了记录正好落在扇区边界的情况**，记录跨扇区时会写进未擦除区域。这是当前代码的一个真实缺陷，已记录在 [docs/03_BUG_REPORT.md](03_BUG_REPORT.md) 第 3 条。

---

## ✅ 验证：串口日志应该长这样

```text
🔥 正在清空历史坏数据...            ← ⚠️ 调试代码，见下方「必须先删掉」
🔥 清空完成！
🆕 [初始状态] 未发现历史指针，从数据区起点开始。
WiFi connected.
System Booted with Deep W25Q64 Storage Ready...

收到实时数据: T:30.30,H:46.00,F:1,V:1,S:1
[断网缓存] 写入W25Q64...            ← 断网容灾开始
...
🔋 [断电恢复] 成功! WriteAddr: 0x00102A ReadAddr: 0x001000   ← 拔电重启后指针恢复
发现离线数据，开始补发...
⚠️ 检测到断电撕裂数据，跳过坏块，防止死循环...                ← 自愈（有坏块时才会出现）
离线数据补发完成。
收到实时数据: T:30.30,H:46.00,F:1,V:1,S:1                    ← 无缝继续
```

树莓派侧同时订阅即可确认补发成功：

```bash
mosquitto_sub -h localhost -t "home/fan/status" -v
```

---

## 🚧 踩坑与排错

### 0.（必做）烧录后立刻删掉「清空坏数据」的调试代码

```cpp
// ⚠️【清空坏数据专用】第一次烧录时，强制擦除元数据区和第一块数据区
Serial.println("🔥 正在清空历史坏数据...");
flashEraseSector(META_SECTOR_START);
flashEraseSector(DATA_SECTOR_START);
Serial.println("🔥 清空完成！");
```

这段是为了清掉早期实验留下的坏数据，但它**每次上电都会执行**：元数据区被擦 → `loadPointers()` 永远返回「初始状态」，**断电记忆功能直接失效**。

**正确做法**：看到第一次 `🆕` 输出后立刻注释掉这三行再重新烧录；或加一个 `#define FACTORY_RESET 0` 开关包起来。详见 [docs/03_BUG_REPORT.md](03_BUG_REPORT.md) 第 1 条。

### 1. 上传报 `No serial data received` / 芯片进不了下载模式

**原因**：`CS` 接在了 GPIO5 / GPIO12 / GPIO15 这类 **Strapping 引脚**上，上电被外部电路拉偏，芯片无法进入下载模式。
**解决**：`CS` 换到 **GPIO13**（并避开 GPIO6~11 内部 Flash 引脚），先读 JEDEC ID 确认 `0xEF 0x40 0x17`。

### 2. `发现离线数据，开始补发...` 无限刷屏

**原因**：读到空记录（`0xFF`）后没有移动读指针，`while` 条件永真。
**解决**：加入「撕裂写入」检测（本篇第四步），读到空数据就 `currentReadAddr = currentWriteAddr; break;`。

### 3. 元数据区很快写满、Flash 寿命堪忧

**原因**：最初每写一条数据就**擦一次元数据扇区**再写指针，4KB 扇区几秒钟就被磨穿。
**解决**：改成**日志追加式**（16 字节/条），写满 256 条（4KB）才擦一次；数据区同理，写满 4KB 才擦。

### 4. 写完读出来是乱码 / 后一条数据覆盖前一条

**原因**：Flash **只能把 1 写成 0**，写之前必须先擦。若记录跨了 256 字节的 Page 边界，`Page Program` 会在页内**回卷**（后半段写到本页开头），把前面的数据覆盖掉。
**解决**：单条记录不要超过页边界，或把一次写拆成多次不跨页的写。详见 [docs/03_BUG_REPORT.md](03_BUG_REPORT.md) 第 2 条。

### 5. 擦除后立刻写，偶尔丢数据

**原因**：`flashEraseSector()` 里只 `delay(100)`，而 W25Q64 的 4KB 扇区擦除**典型 45ms、最大 400ms**，忙的时候写命令会被忽略。
**解决**：改成轮询状态寄存器 `0x05` 的 BUSY 位（bit0）直到清零再返回。

---

## 🔎 原理小结

| 机制 | 解决的问题 | 关键实现 |
| --- | --- | --- |
| 外置 W25Q64 | 容量 + 磨损 + 不伤主控 Flash | 裸 SPI：`0x20` / `0x02` / `0x03` |
| CS = GPIO13 | 避开 Strapping 导致无法下载 | 先读 JEDEC ID 自检 |
| 扇区规划 | 元数据与数据分离，支持环形复用 | Sector0 元数据 + Sector1 起数据区 |
| 日志追加 + CRC8 | 断电恢复指针；顺带降低磨损 256 倍 | 16 字节记录 / 写满才擦 |
| **撕裂自愈** | 拔电导致补发死循环 | 空记录 → 指针强制对齐 → `break` |
| `std::queue` | `publish` 阻塞不再拖垮串口读取 | 读串口只入队 |

**一句话总结：Flash 的关键不是「写进去」，而是「擦干净再写」和「写坏了能跳过去」。**

---

## 🚀 继续往下

- [ ] 记录加 2 字节长度 + CRC8，彻底区分「半写」与「完整」
- [ ] 补发循环内调用 `client.loop()` 并检查 `publish` 返回值（长补发会被 MQTT keepalive 断开）
- [ ] 环形缓冲加容量保护（写指针追上读指针前停止写入并告警）
- [ ] 用状态寄存器轮询替代 `delay()`
- [ ] 动态客户端 ID（`ESP32Client-xxxx`），避免多网关互相踢线
- [ ] 缓存记录改成 JSON，附带时间戳便于树莓派侧排序

## 🔗 参考链接

- [W25Q64 datasheet（Winbond）](https://www.winbond.com/resource-files/w25q64jv%20revj%2003272018%20plus.pdf)
- [ESP32 Strapping 引脚说明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/gpio.html)
- [ESP32 SPI 库文档](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/spi.html)
- 同系列：[docs/01 断网缓存（SPIFFS）与非阻塞队列](01_OFFLINE_CACHE_AND_QUEUE.md)
- 本篇缺陷清单：[docs/03_BUG_REPORT.md](03_BUG_REPORT.md)
- 配套仓库：[STM32（执行终端）](https://github.com/Arduino-STM32-Dev/STM32)
