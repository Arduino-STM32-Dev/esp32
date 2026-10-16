#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <queue>

// --- 引脚定义 ---
#define FLASH_CS   13
#define FLASH_SCK  18
#define FLASH_MISO 19
#define FLASH_MOSI 23

// --- 存储区定义 ---
#define META_SECTOR_START 0x000000    // 元数据扇区 (4KB)
#define DATA_SECTOR_START 0x001000    // 数据区起始地址
#define FLASH_MAX_ADDR    0x7FFFFF    // 8MB 最大地址
#define FLASH_SECTOR_SIZE 4096        // 每次擦除4KB

// --- WiFi 和 MQTT 配置 ---
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* mqtt_server = "YOUR_PI_IP";
const int mqtt_port = 1883;
const char* mqtt_topic_pub = "home/fan/status"; 
const char* mqtt_topic_sub = "pi/to/esp32";     

WiFiClient espClient;
PubSubClient client(espClient);
HardwareSerial SerialSTM(2); 
String receivedData = "";
std::queue<String> dataQueue; 

bool isSyncing = false; 
unsigned long lastReconnectAttempt = 0;

// 当前读写指针
uint32_t currentWriteAddr = DATA_SECTOR_START; 
uint32_t currentReadAddr = DATA_SECTOR_START;  

// ==========================================
// CRC8 校验函数
// ==========================================
uint8_t crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    uint8_t extract = *data++;
    for (uint8_t tempI = 8; tempI; tempI--) {
      uint8_t sum = (crc ^ extract) & 0x01;
      crc >>= 1;
      if (sum) crc ^= 0x8C;
      extract >>= 1;
    }
  }
  return crc;
}

// ==========================================
// W25Q64 底层驱动函数
// ==========================================
void flashEraseSector(uint32_t addr) {
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x20); // Sector Erase (4KB)
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  digitalWrite(FLASH_CS, HIGH);
  delay(100); 
}

void flashWriteString(uint32_t addr, String data) {
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x02); // Page Program
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  for (uint16_t i = 0; i < data.length(); i++) {
    SPI.transfer(data[i]);
  }
  digitalWrite(FLASH_CS, HIGH);
  delay(5); 
}

String flashReadString(uint32_t addr) {
  String data = "";
  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x03); // Read Data
  SPI.transfer((addr >> 16) & 0xFF);
  SPI.transfer((addr >> 8) & 0xFF);
  SPI.transfer(addr & 0xFF);
  for (uint16_t i = 0; i < 256; i++) {
    char c = SPI.transfer(0x00);
    if (c == '\n' || c == 0xFF) break; 
    data += c;
  }
  digitalWrite(FLASH_CS, HIGH);
  return data;
}

// ==========================================
// 断电记忆指针保存与恢复
// ==========================================
void savePointers() {
  uint8_t record[16];
  memset(record, 0xFF, 16); 
  
  static uint8_t seq = 0;
  seq++;
  
  record[0] = 0xA5;
  record[1] = seq;
  record[2] = (currentWriteAddr >> 24) & 0xFF;
  record[3] = (currentWriteAddr >> 16) & 0xFF;
  record[4] = (currentWriteAddr >> 8) & 0xFF;
  record[5] = currentWriteAddr & 0xFF;
  record[6] = (currentReadAddr >> 24) & 0xFF;
  record[7] = (currentReadAddr >> 16) & 0xFF;
  record[8] = (currentReadAddr >> 8) & 0xFF;
  record[9] = currentReadAddr & 0xFF;
  record[10] = crc8(record, 10);

  uint32_t metaAddr = META_SECTOR_START;
  while (metaAddr < META_SECTOR_START + FLASH_SECTOR_SIZE) {
    uint8_t magic = 0xFF;
    digitalWrite(FLASH_CS, LOW);
    SPI.transfer(0x03);
    SPI.transfer((metaAddr >> 16) & 0xFF);
    SPI.transfer((metaAddr >> 8) & 0xFF);
    SPI.transfer(metaAddr & 0xFF);
    magic = SPI.transfer(0x00);
    digitalWrite(FLASH_CS, HIGH);

    if (magic == 0xFF) break; 
    metaAddr += 16;
  }

  if (metaAddr >= META_SECTOR_START + FLASH_SECTOR_SIZE) {
    flashEraseSector(META_SECTOR_START);
    metaAddr = META_SECTOR_START;
  }

  digitalWrite(FLASH_CS, LOW);
  SPI.transfer(0x02);
  SPI.transfer((metaAddr >> 16) & 0xFF);
  SPI.transfer((metaAddr >> 8) & 0xFF);
  SPI.transfer(metaAddr & 0xFF);
  for (int i = 0; i < 16; i++) SPI.transfer(record[i]);
  digitalWrite(FLASH_CS, HIGH);
  delay(5);
}

void loadPointers() {
  uint32_t lastValidWrite = DATA_SECTOR_START;
  uint32_t lastValidRead = DATA_SECTOR_START;
  bool found = false;

  for (uint32_t addr = META_SECTOR_START; addr < META_SECTOR_START + FLASH_SECTOR_SIZE; addr += 16) {
    uint8_t record[16];
    digitalWrite(FLASH_CS, LOW);
    SPI.transfer(0x03);
    SPI.transfer((addr >> 16) & 0xFF);
    SPI.transfer((addr >> 8) & 0xFF);
    SPI.transfer(addr & 0xFF);
    for (int i = 0; i < 16; i++) record[i] = SPI.transfer(0x00);
    digitalWrite(FLASH_CS, HIGH);

    if (record[0] == 0xA5 && record[10] == crc8(record, 10)) {
      lastValidWrite = (record[2] << 24) | (record[3] << 16) | (record[4] << 8) | record[5];
      lastValidRead  = (record[6] << 24) | (record[7] << 16) | (record[8] << 8) | record[9];
      found = true;
    }
  }

  if (found) {
    currentWriteAddr = lastValidWrite;
    currentReadAddr = lastValidRead;
    Serial.print("🔋 [断电恢复] 成功! WriteAddr: 0x"); Serial.print(currentWriteAddr, HEX);
    Serial.print(" ReadAddr: 0x"); Serial.println(currentReadAddr, HEX);
  } else {
    currentWriteAddr = DATA_SECTOR_START;
    currentReadAddr = DATA_SECTOR_START;
    Serial.println("🆕 [初始状态] 未发现历史指针，从数据区起点开始。");
  }
}

// ==========================================
// 网络与回调
// ==========================================
void callback(char* topic, byte* payload, unsigned int length) {
  String cmd;
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.print("Received Command: "); Serial.println(cmd);
  SerialSTM.println(cmd); 
}

void setup_wifi() {
  delay(10);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected.");
}

void attemptReconnect() {
  unsigned long now = millis();
  if (now - lastReconnectAttempt > 5000) { 
    lastReconnectAttempt = now;
    if (client.connect("ESP32Client")) {
      client.subscribe(mqtt_topic_sub);
      Serial.println("MQTT connected");
    }
  }
}

// ==========================================
// 初始化与主循环
// ==========================================
void setup() {
  Serial.begin(115200); 
  
  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);
  SPI.begin(FLASH_SCK, FLASH_MISO, FLASH_MOSI, FLASH_CS);
  SPI.setFrequency(1000000); 
  
  // ⚠️【清空坏数据专用】第一次烧录时，强制擦除元数据区和第一块数据区
  // 烧录完毕并看到串口打印 🆕 后，请立刻把下面两行注释掉，再重新烧录！
  Serial.println("🔥 正在清空历史坏数据...");
  flashEraseSector(META_SECTOR_START);
  flashEraseSector(DATA_SECTOR_START);
  Serial.println("🔥 清空完成！");
  // =========================================================
  
  loadPointers();

  SerialSTM.setRxBufferSize(1024); 
  SerialSTM.begin(9600, SERIAL_8N1, 16, 17); 

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); 
  Serial.println("System Booted with Deep W25Q64 Storage Ready...");
}

void loop() {
  if (!client.connected()) attemptReconnect();
  else client.loop();

  // 1. 读取STM32数据并放入队列
  while (SerialSTM.available()) {
    char c = SerialSTM.read();
    if (c == '\n') { 
      if (receivedData.length() > 0) {
        dataQueue.push(receivedData);
        Serial.println("收到实时数据: " + receivedData); 
        receivedData = ""; 
      }
    } else if (c != '\r') {
      receivedData += c; 
    }
  }

  // 2. 处理队列数据
  if (!dataQueue.empty()) {
    if (WiFi.status() == WL_CONNECTED && client.connected()) {
      String data = dataQueue.front();
      dataQueue.pop();
      client.publish(mqtt_topic_pub, data.c_str());
    } else {
      Serial.println("[断网缓存] 写入W25Q64...");
      while (!dataQueue.empty()) {
        String data = dataQueue.front();
        dataQueue.pop();
        
        if (currentWriteAddr % FLASH_SECTOR_SIZE == 0) {
           flashEraseSector(currentWriteAddr);
        }
        
        flashWriteString(currentWriteAddr, data);
        currentWriteAddr += data.length() + 1; 
        
        if (currentWriteAddr > FLASH_MAX_ADDR) {
           currentWriteAddr = DATA_SECTOR_START;
        }
        savePointers();
      }
    }
  }

  // 3. 离线数据补发（修复死循环的核心逻辑）
  if (WiFi.status() == WL_CONNECTED && client.connected() && !isSyncing) {
    if (currentReadAddr != currentWriteAddr) {
      isSyncing = true; 
      Serial.println("发现离线数据，开始补发...");
      
      while (currentReadAddr != currentWriteAddr) {
        if (currentReadAddr > FLASH_MAX_ADDR) {
          currentReadAddr = DATA_SECTOR_START;
        }
        
        String line = flashReadString(currentReadAddr);
        if (line.length() > 0) {
          client.publish(mqtt_topic_pub, line.c_str());
          currentReadAddr += line.length() + 1;
          savePointers(); 
          delay(30); 
        } else {
          // 【关键修复】检测到空数据（0xFF），说明拔电时这块写坏了
          Serial.println("⚠️ 检测到断电撕裂数据，跳过坏块，防止死循环...");
          currentReadAddr = currentWriteAddr; // 强制对齐读写指针
          savePointers(); 
          break; // 跳出补发循环
        }
      }
      Serial.println("离线数据补发完成。");
      isSyncing = false; 
    }
  }
}