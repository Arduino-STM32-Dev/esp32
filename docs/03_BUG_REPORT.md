# ESP32 代码审计报告 · Bug Finder

> **审计对象**：`firmware/external_cache/external_cache.ino`（317 行，W25Q64 断电记忆网关）
> **参照对象**：`firmware/cache_control/cache_control.ino`（SPIFFS 版）、`firmware/esp32_test/esp32_test.ino`（基础版）
> **审计方式**：逐行人工审查 + 静态规则扫描（文末附自查命令）
> **审计结论**：**功能可用，但存在 2 个致命问题、4 个高危问题**。其中 P0-1 会让「断电记忆」在每次上电时被自己抹掉，**必须立刻修**。

---

## 📋 缺陷总表

| 编号 | 等级 | 位置 | 一句话描述 |
| --- | --- | --- | --- |
| **P0-1** | 🔴 致命 | `external_cache.ino:224-230` | setup() 每次上电强制擦除元数据区与首数据扇区，**断电记忆功能直接失效** |
| **P0-2** | 🔴 致命 | `external_cache.ino:69-80` + `:277` | Page Program 跨 256 字节页边界会**在页内回卷**，静默覆盖已写数据 |
| **P1-1** | 🟠 高危 | `external_cache.ino:273-275` | 只用「地址正好整除 4096」判断擦除，记录跨扇区时写入**未擦除区域** → 数据损坏 |
| **P1-2** | 🟠 高危 | `external_cache.ino:294-312` | 长补发循环内不调用 `client.loop()`，超过 MQTT keepalive 会被踢线，后续 publish **静默失败** |
| **P1-3** | 🟠 高危 | `external_cache.ino:206` | MQTT 客户端 ID 硬编码 `"ESP32Client"`，多网关/僵尸会话互相踢线（旧版用 `random()`，属**功能回归**） |
| **P1-4** | 🟠 高危 | `external_cache.ino:280-282` | 环形缓冲无容量保护，写指针追上读指针即**覆盖未补发数据**，无告警 |
| **P2-1** | 🟡 中 | `external_cache.ino:66,79,147` | 只靠 `delay()` 等擦写完成，无 BUSY 轮询；W25Q64 擦除最大 400ms > `delay(100)` |
| **P2-2** | 🟡 中 | `external_cache.ino:82-96` | 记录无长度/CRC，**半写的记录会被当成完整行 publish**（脏数据上云） |
| **P2-3** | 🟡 中 | `external_cache.ino:150-182` | 恢复的指针**无范围校验**，撕裂数据若 CRC 恰好命中 → 指针越界 → 立刻擦错扇区 |
| **P2-4** | 🟡 中 | `external_cache.ino:187-193` | `callback()` 不校验 payload 长度与内容，超长/非法指令直通 STM32 串口 |
| **P3-1** | 🔵 低 | `external_cache.ino:29-30,257` | 全局 `String` + `std::queue<String>` 频繁拼接 → 堆碎片，长期运行有分配失败风险 |
| **P3-2** | 🔵 低 | `external_cache.ino:105-108` | `static uint8_t seq` 自增后回绕，且 `loadPointers()` 从不读它 → 死字段 |
| **P3-3** | 🔵 低 | `external_cache.ino:308` | 撕裂自愈把坏记录**之后**的所有数据一并丢弃（取舍，非错误，但应知悉） |
| **P3-4** | 🔵 低 | `external_cache.ino:198` | `setup_wifi()` 里 `while (WiFi.status() != WL_CONNECTED)` 仍是**阻塞等待**，无超时 |

---

## 🔴 P0-1：每次上电都会抹掉断电记忆（致命）

**位置**：`external_cache.ino:224-230`

```cpp
  // ⚠️【清空坏数据专用】第一次烧录时，强制擦除元数据区和第一块数据区
  // 烧录完毕并看到串口打印 🆕 后，请立刻把下面两行注释掉，再重新烧录！
  Serial.println("🔥 正在清空历史坏数据...");
  flashEraseSector(META_SECTOR_START);
  flashEraseSector(DATA_SECTOR_START);
  Serial.println("🔥 清空完成！");
```

**症状**：串口每次启动都打印 `🔥 正在清空历史坏数据...` → `🆕 [初始状态] 未发现历史指针`，即使上一轮明明缓存了数据、也明明打印过 `🔋 [断电恢复] 成功!`。表现为「断电记忆时好时坏 / 永远不生效」。

**根因**：这段是排障用的**一次性**代码，但被提交进了正式固件。`loadPointers()` 在它之后执行，元数据区刚被擦成 `0xFF`，自然扫不到任何有效记录。数据区首扇区也被擦，等于「上电即清空」。

**复现**：断网让 ESP32 写入几条数据（看到 `[断网缓存] 写入W25Q64...`）→ 按 RESET（不拔电）→ 观察串口。若仍打印 `🔥` 与 `🆕`，即命中。

**修复**（任选其一）：

```cpp
// 方案 A（推荐）：编译期开关，默认关闭
#define FACTORY_RESET 0

void setup() {
  ...
#if FACTORY_RESET
  Serial.println("🔥 正在清空历史坏数据...");
  flashEraseSector(META_SECTOR_START);
  flashEraseSector(DATA_SECTOR_START);
  Serial.println("🔥 清空完成！");
#endif
  loadPointers();
  ...
}
```

```cpp
// 方案 B（最简）：直接注释掉这三行，并保留注释说明为什么不能留
  // flashEraseSector(META_SECTOR_START);   // ⚠️ 一次性调试用，留着会清空断电记忆
  // flashEraseSector(DATA_SECTOR_START);
```

**验证**：写入数据 → 拔电 → 上电，必须看到 `🔋 [断电恢复] 成功!`，且不再出现 `🔥`。

---

## 🔴 P0-2：Page Program 跨页回卷，静默覆盖数据

**位置**：`external_cache.ino:69-80`（写入函数）与 `:277`（调用处 `flashWriteString(currentWriteAddr, data)`）

**症状**：数据区里较早的记录被「最新的记录」覆盖；读出来是拼接错乱的字符串，例如 `T:30.30,H:46.00T:30.30,H:46.00,F:1`。

**根因**：W25Q64 的 **Page Program（0x02）只能在一页（256 字节）内写**，且**地址到达页尾后会在页内回卷**——不是自动进到下一页。`flashWriteString()` 直接把 `data.length()` 个字节一口气 `SPI.transfer()` 出去：

```cpp
for (uint16_t i = 0; i < data.length(); i++) {
  SPI.transfer(data[i]);        // 若起始地址 + i 越过 256 字节页边界 → 回卷到页首
}
```

例如地址 `0x0010F0`（页内偏移 240）写一条 40 字节的记录：后 16 字节会写到 `0x001000`，**把这条扇区开头的老数据覆盖掉**。而且回卷是静默的，函数无任何报错。

**复现**：写一条足够长的数据并让地址靠近页尾，然后从前一页页首读回，能看到被覆盖的字节。

**修复**：按页切分写入。

```cpp
void flashWriteString(uint32_t addr, String data) {
  uint16_t written = 0;
  while (written < data.length()) {
    uint16_t pageOffset = (addr + written) & 0xFF;         // 页内偏移
    uint16_t room = 256 - pageOffset;                      // 本页剩余空间
    uint16_t chunk = min<uint16_t>(room, data.length() - written);
    if (chunk == 0) break;

    digitalWrite(FLASH_CS, LOW);
    SPI.transfer(0x02);                                    // Page Program
    uint32_t a = addr + written;
    SPI.transfer((a >> 16) & 0xFF);
    SPI.transfer((a >> 8)  & 0xFF);
    SPI.transfer( a        & 0xFF);
    for (uint16_t i = 0; i < chunk; i++) SPI.transfer(data[written + i]);
    digitalWrite(FLASH_CS, HIGH);
    waitFlashReady();                                      // 见 P2-1 的修复
    written += chunk;
  }
}
```

**验证**：把 `currentWriteAddr` 手动设到 `0x0010F0` 附近写一条 64 字节记录，读回应与原文一致，且前一页页首数据未被改写。

---

## 🟠 P1-1：「对齐才擦除」导致跨扇区写入未擦除区域

**位置**：`external_cache.ino:273-275`

```cpp
if (currentWriteAddr % FLASH_SECTOR_SIZE == 0) {
   flashEraseSector(currentWriteAddr);
}
flashWriteString(currentWriteAddr, data);
currentWriteAddr += data.length() + 1;
```

**症状**：断网缓存一段时间后，补发出来的数据里混进**上上轮的老数据**（旧记录残留字节被当成新内容），或新记录开头正常、尾部异常。

**根因**：两条独立缺陷叠加：

1. 擦除判据只覆盖「地址**正好**是 4096 倍数」的情形。记录长度为 `data.length() + 1`，**几乎永远不会正好落在扇区边界上**，所以「跨扇区」是常态而不是例外；
2. Flash 只能把 `1` 写成 `0`。写进一个**没擦过**的扇区，等于在旧数据上做按位与 → 结果既不是旧值也不是新值，而且旧数据里若没有 `0xFF`，`flashReadString()` 就一直读到 256 字节上限才停，把一堆垃圾当成一条记录。

**修复**：写之前计算本次写入覆盖的扇区范围，对新进入的扇区先擦；**擦之前必须确认该扇区没有待补发数据**（否则会擦掉还没发出去的缓存），这是环形缓冲「写指针不能越过读指针」的硬约束。

```cpp
// 仅示意：进入新扇区前擦除，且绝不擦到「读指针所在扇区」
uint32_t startSector = currentWriteAddr        & ~(FLASH_SECTOR_SIZE - 1);
uint32_t endSector   = (currentWriteAddr + data.length()) & ~(FLASH_SECTOR_SIZE - 1);

for (uint32_t s = startSector; s <= endSector; s += FLASH_SECTOR_SIZE) {
  if (s == (currentReadAddr & ~(FLASH_SECTOR_SIZE - 1))) break;  // 别擦掉待补发数据
  if (isSectorAllFF(s)) continue;                                 // 已经是 0xFF 就不必再擦
  flashEraseSector(s);
  waitFlashReady();
}
```

**验证**：连续写入超过 4KB 数据（跨多个扇区）后断电重启，补发内容应与写入内容逐条一致，无老数据混入。

---

## 🟠 P1-2：长补发期间 MQTT 掉线，publish 静默失败

**位置**：`external_cache.ino:294-312`

```cpp
while (currentReadAddr != currentWriteAddr) {
  String line = flashReadString(currentReadAddr);
  if (line.length() > 0) {
    client.publish(mqtt_topic_pub, line.c_str());   // ← 返回值被忽略
    currentReadAddr += line.length() + 1;
    savePointers();                                  // ← 内部 delay(5)
    delay(30);                                       // ← 每条 30ms
  }
  ...
}
```

**症状**：补发几千条后，串口打印 `离线数据补发完成。`，但树莓派只收到前面一部分；或补发中途 WiFi 图标还亮着，`client.connected()` 已经为 false。

**根因**：

- `client.loop()` 只在 `loop()` 顶部的 `else` 分支里被调用一次；进入补发 `while` 后**长时间不再调用**，MQTT 报文（PINGREQ）也无法收发；
- PubSubClient 默认 **keepalive = 15 秒**，Broker 在 1.5 倍时间内收不到心跳就断开；
- 每条记录 `delay(30)` + `savePointers()` 内 `delay(5)` + 擦除 `delay(100)`，2000 条 ≈ **70 秒**，远超 15 秒；
- `client.publish()` 的返回值没检查，**连接已断时它返回 false 但数据被视为「已补发」**，`currentReadAddr` 照样前移 → 数据永久丢失。

**修复**：

```cpp
while (currentReadAddr != currentWriteAddr) {
  if (!client.connected()) {                    // 连接断了就停下，指针保留
    Serial.println("⚠️ 补发中断：MQTT 已断开，保留指针下次继续。");
    break;
  }
  client.loop();                                // 喂 keepalive
  ...
  if (client.publish(mqtt_topic_pub, line.c_str())) {
    currentReadAddr += line.length() + 1;       // 只有 publish 成功才推进指针
    savePointers();
  } else {
    Serial.println("⚠️ publish 失败，指针不推进，稍后重试。");
    break;
  }
  delay(30);
}
```

**验证**：制造 5000 条离线数据后恢复网络，观察补发期间 `client.connected()` 始终为真；人为在补发中断网，串口应打印「补发中断」而不是「补发完成」。

---

## 🟠 P1-3：MQTT 客户端 ID 硬编码（功能回归）

**位置**：`external_cache.ino:206`

```cpp
if (client.connect("ESP32Client")) {
```

**症状**：同时开两台网关时，两边**轮流掉线重连**（串口每几秒刷一次 `MQTT connected`）；或者上电后一直连不上，直到 Broker 上那条僵尸会话超时被清掉。

**根因**：MQTT 规范规定 **同一 Client ID 的第二个连接会踢掉第一个**。硬编码 ID 让所有设备争抢同一个身份。旧版 `cache_control.ino` 用的是 `clientId += String(random(0xffff), HEX);`，升级到 W25Q64 版本时**丢失了唯一性**。

**修复**：

```cpp
String clientId = "ESP32Client-" + String((uint32_t)ESP.getEfuseMac(), HEX);
client.connect(clientId.c_str());
```

**验证**：两台 ESP32 同时上线，Broker 侧 `mosquitto_sub -t '$SYS/broker/clients/connected'` 应稳定为 2，双方都不再反复重连。

---

## 🟠 P1-4：环形缓冲无容量保护，静默覆盖未补发数据

**位置**：`external_cache.ino:280-282`

```cpp
if (currentWriteAddr > FLASH_MAX_ADDR) {
   currentWriteAddr = DATA_SECTOR_START;     // 回卷，可能追上甚至越过读指针
}
```

**症状**：长时间断网（写满 8MB）后恢复，补发条数明显少于断网期间产生的条数，没有任何告警。

**根因**：只有「写指针越界」一个退出条件，**没有「写指针是否已追上读指针」的检查**。一旦追上，新数据会覆盖**还没补发**的旧数据，而读指针仍指向被覆盖处的中间，读出来的内容是错乱的混合体。

**修复**：写入前预留安全距离（例如保留一个扇区），满了就告警并丢弃**最旧**的数据（同时推进读指针），而不是覆盖一半。

```cpp
uint32_t nextWrite = currentWriteAddr + data.length() + 1;
uint32_t guard = FLASH_SECTOR_SIZE * 2;                     // 预留 2 个扇区的安全距离
if (nextWrite > FLASH_MAX_ADDR) nextWrite = DATA_SECTOR_START;

if (writeWouldOverrun(nextWrite)) {
  Serial.println("🛑 缓存已满！丢弃最旧数据以保护完整性。");
  // 方案：推进 currentReadAddr 到 (nextWrite + guard) 对齐的下一个扇区起点，并 savePointers()
}
```

**验证**：把 `FLASH_MAX_ADDR` 临时改小（如 `0x001FFF`）快速写满，串口应出现「缓存已满」告警，且读数不出现拼接错乱。

---

## 🟡 P2-1：无 BUSY 轮询，`delay()` 是赌博

**位置**：`external_cache.ino:66`（`delay(100)`）、`:79`（`delay(5)`）、`:147`（`delay(5)`）

**问题**：W25Q64 的 4KB 扇区擦除**典型耗时 45ms、最大 400ms**，页编程典型 0.7ms、最大 3ms。`delay(100)` 覆盖不了最坏情况；擦除未完成时下发的 `0x02` 会被芯片**直接忽略**，表现为「偶尔丢一条数据」。

**修复**：读状态寄存器 1（`0x05`）的 BUSY 位（bit0），忙则等待。

```cpp
void waitFlashReady() {
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x05);                       // Read Status Register-1
  while (SPI.transfer(0x00) & 0x01) {       // bit0 = BUSY
    // 忙等；可加超时保护防止死循环
  }
  digitalWrite(FLASH_CS, HIGH);
}
```

---

## 🟡 P2-2：半写记录被当成完整行上报

**位置**：`external_cache.ino:82-96`（读）与 `:69-80`（写）

**问题**：记录结束只靠 `0xFF`。若断电发生在记录写入中途，前几个字节是**有效 ASCII**、后面才是 `0xFF` —— `flashReadString()` 会原样返回这半条记录，`publish` 出去就是脏数据（例如 `T:30.3` 少了一半字段，树莓派解析错位却看不出错）。

**修复**：给每条记录加固定头部：`[2 字节长度][1 字节 CRC8][N 字节内容]`。读取时先校验长度与 CRC，失败即按撕裂处理。

```cpp
// 写入：len(2) + crc8(1) + payload
// 读取：校验失败 → 视为坏块，跳过
```

---

## 🟡 P2-3：恢复的指针没有范围校验

**位置**：`external_cache.ino:150-182`

**问题**：`loadPointers()` 只校验 Magic + CRC8，**不校验数值范围**。若撕裂写入让指针字段写入了合法 CRC 但数值荒谬（Flash 回卷、位翻转），恢复后：

- `currentWriteAddr` 可能远大于 `FLASH_MAX_ADDR`，下一轮写入时 `flashEraseSector()` 会**擦到错误地址**；
- 或读指针 > 写指针，导致补发逻辑行为不可预测。

**修复**：

```cpp
bool inRange(uint32_t a) { return a >= DATA_SECTOR_START && a <= FLASH_MAX_ADDR; }
if (record[0] == 0xA5 && record[10] == crc8(record, 10)) {
  uint32_t w = ..., r = ...;
  if (inRange(w) && inRange(r)) { lastValidWrite = w; lastValidRead = r; found = true; }
  else Serial.println("⚠️ 元数据指针越界，已忽略该记录。");
}
```

---

## 🟡 P2-4：回调不校验指令长度与内容

**位置**：`external_cache.ino:187-193`

```cpp
void callback(char* topic, byte* payload, unsigned int length) {
  String cmd;
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.print("Received Command: "); Serial.println(cmd);
  SerialSTM.println(cmd);            // 直通 STM32，无长度/白名单校验
}
```

**问题**：① `length` 无上限，超长 payload 会构造超大 `String`，并一次性塞满 STM32 的接收缓冲（STM32 端 `Serial.setTimeout(10)` 只能缓解）；② 无白名单，任何内容都会转发给执行终端——工业场合等于把执行器暴露给任意发消息者。

**修复**：

```cpp
const char* ALLOWED[] = {"FAN_ON","FAN_OFF","VALVE_OPEN","VALVE_CLOSE"};
if (length > 32) { Serial.println("⚠️ 指令过长，已丢弃"); return; }
if (!cmd.startsWith("FAN_PWM:") && !isAllowed(cmd)) { Serial.println("⚠️ 未知指令，已丢弃"); return; }
```

---

## 🔵 低优先级项（P3）

| 编号 | 位置 | 说明与建议 |
| --- | --- | --- |
| P3-1 | `:29-30, :257` | `String receivedData` 反复 `+=` 拼接 + `std::queue<String>` 动态分配，长时间运行会堆碎片。建议改用 `char buf[128]` + 固定长度环形队列。 |
| P3-2 | `:105-108` | `savePointers()` 中 `static uint8_t seq` 只写不读，`loadPointers()` 也不比较序号。要么在 `loadPointers()` 里按 `seq` 取最新记录，要么删掉这个字段以免误导。 |
| P3-3 | `:308` | `currentReadAddr = currentWriteAddr` 会连带丢弃撕裂点**之后**所有已完整写入的数据。若把记录改成「长度 + CRC」结构，可以只跳过错的那一条（P2-2 的修复顺带解决）。 |
| P3-4 | `:198` | `setup_wifi()` 的 `while (WiFi.status() != WL_CONNECTED)` 是阻塞等待，无超时。建议改为 10 秒超时后继续启动，让离线缓存逻辑在无网时也能工作。 |

---

## 🔧 建议修复顺序

| 优先级 | 动作 | 影响面 |
| --- | --- | --- |
| 1 | 移除/开关化 setup() 里的强制擦除（P0-1） | 1 行删除，立刻恢复断电记忆 |
| 2 | `flashWriteString()` 按页切分（P0-2） | 防数据互相覆盖 |
| 3 | 补发循环加 `client.loop()` + 检查 `publish` 返回值（P1-2） | 防长补发丢数据 |
| 4 | 扇区擦除判据改为「跨扇区」判断（P1-1） | 防脏数据混入 |
| 5 | 客户端 ID 唯一化（P1-3） | 1 行，解决多机互踢 |
| 6 | BUSY 轮询 + 指针范围校验（P2-1、P2-3） | 提升长期稳定性 |
| 7 | 记录加长度 + CRC（P2-2） | 彻底解决撕裂与脏数据 |

---

## 🧰 自查清单（提交前跑一遍）

```bash
# 1. 是否残留调试擦除代码
grep -n "清空历史坏数据\|flashEraseSector(META_SECTOR_START)" firmware/external_cache/external_cache.ino

# 2. 是否还有硬编码客户端 ID
grep -n 'connect("' firmware/external_cache/external_cache.ino

# 3. publish 返回值是否被检查（应全部包在 if 中）
grep -n "client.publish" firmware/external_cache/external_cache.ino

# 4. 补发循环内是否有 client.loop()
grep -n "client.loop()" firmware/external_cache/external_cache.ino

# 5. 是否存在无 BUSY 轮询的裸 delay
grep -n "delay(" firmware/external_cache/external_cache.ino

# 6. 敏感信息（WiFi 名称 / 密码 / 内网 IP）是否已清理
#    ⚠️ 这里故意不写真实字符串，请把你自己的 SSID / 密码 / IP 关键词填进去再跑
grep -rn -e "<你的WiFi名称>" -e "<你的WiFi密码>" -e "192.168." firmware/ docs/ *.md
```

| 期望结果 | 说明 |
| --- | --- |
| 第 1 条：无输出或仅在 `#if` 内 | 否则断电记忆失效 |
| 第 2 条：无 `connect("ESP32Client")` | 应为动态 ID |
| 第 3 条：每处都在 `if (...)` 内 | 未检查返回值 = 丢数据不可见 |
| 第 4 条：补发 `while` 内也有一处 | 否则长补发被踢线 |
| 第 5 条：擦写后为 `waitFlashReady()` | 而非裸 `delay()` |
| 第 6 条：无输出 | 凭证不得入库 |

---

## 📎 附：本次审计涉及的关键代码位置对照

| 功能 | 行号 |
| --- | --- |
| 引脚定义（CS=13 / SCK=18 / MISO=19 / MOSI=23） | `:7-10` |
| 扇区规划常量 | `:13-16` |
| `crc8()` | `:42-54` |
| `flashEraseSector()` / `flashWriteString()` / `flashReadString()` | `:59-96` |
| `savePointers()` / `loadPointers()` | `:101-182` |
| `callback()` / `setup_wifi()` / `attemptReconnect()` | `:187-211` |
| `setup()`（含 🔥 调试擦除） | `:216-241` |
| `loop()`：读串口入队 | `:247-259` |
| `loop()`：在线发布 / 离线写 Flash | `:261-286` |
| `loop()`：离线补发与撕裂自愈 | `:288-316` |

> 相关教程：[docs/02 W25Q64 断电记忆缓存与撕裂写入自愈](02_W25Q64_POWER_LOSS_CACHE.md)
> 姊妹报告：[STM32 代码审计报告](https://github.com/Arduino-STM32-Dev/STM32/blob/main/docs/08_BUG_REPORT.md)
