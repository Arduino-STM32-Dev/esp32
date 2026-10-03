#include <WiFi.h>
#include <PubSubClient.h>

// ===================== 【1. 必须修改的参数】 =====================
const char* ssid = "";          // 修改为你的 WiFi 名称
const char* password = "";      // 修改为你的 WiFi 密码
const char* mqtt_server = ""; // 修改为你的树莓派 IP 地址
// ===============================================================

// 定义硬件引脚 (根据你核心板的丝印，对应 D16 和 D17)
#define RXD2 16
#define TXD2 17

// 初始化 WiFi 和 MQTT 对象
WiFiClient espClient;
PubSubClient client(espClient);

// 用于定时发送心跳/状态
unsigned long lastMsg = 0;

// ===================== 【2. WiFi 连接函数】 =====================
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("正在连接 WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi 连接成功！");
  Serial.print("ESP32 的 IP 地址: ");
  Serial.println(WiFi.localIP());
}

// ===================== 【3. MQTT 接收回调 (树莓派 -> ESP32)】 =====================
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("收到树莓派指令 [");
  Serial.print(topic);
  Serial.print("]: ");

  String command = "";
  for (int i = 0; i < length; i++) {
    command += (char)payload[i];
  }
  Serial.println(command);

  // 将收到的指令通过 Serial2 转发给 STM32
  // 注意：这里用 println，STM32 的 readStringUntil('\n') 才能正确解析
  Serial2.println(command);
}

// ===================== 【4. MQTT 重连机制】 =====================
void reconnect() {
  // 不断重试，直到连上 MQTT 服务器
  while (!client.connected()) {
    Serial.print("尝试连接 MQTT...");
    // 尝试连接，客户端 ID 采用 ESP32 MAC 地址，防止重复
    String clientId = "ESP32Client-" + String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str())) {
      Serial.println("已连接");
      // 连接成功后，订阅树莓派下发的主题
      client.subscribe("pi/to/esp32");
    } else {
      Serial.print("连接失败, 状态码 rc=");
      Serial.print(client.state());
      Serial.println(" 5秒后重试");
      delay(5000);
    }
  }
}

// ===================== 【5. setup 初始化】 =====================
void setup() {
  // 电脑调试串口
  Serial.begin(115200);
  
  // 与 STM32 通信的串口 (RX=16, TX=17)
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);

  // 连接 WiFi
  setup_wifi();
  
  // 设置 MQTT 服务器地址和端口
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  
  Serial.println("初始化完成，等待连接...");
}

// ===================== 【6. loop 主循环】 =====================
void loop() {
  // 确保 MQTT 处于连接状态
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // 必须保留，用于处理 MQTT 心跳和接收消息

  // ===================== 【7. 读取 STM32 的回传数据，并上报给树莓派】 =====================
  // 如果 Serial2 有数据传来 (来自 STM32)
  if (Serial2.available() > 0) {
    String stm32Data = Serial2.readStringUntil('\n');
    stm32Data.trim(); // 清除回车换行符
    
    if (stm32Data.length() > 0) {
      Serial.print("收到 STM32 回传: ");
      Serial.println(stm32Data);
      
      // 将 STM32 的数据发布回树莓派 (树莓派可以订阅 esp32/status 主题)
      client.publish("esp32/status", stm32Data.c_str());
    }
  }

  // ===================== 【8. 定时发送心跳或测试数据】 =====================
  unsigned long now = millis();
  if (now - lastMsg > 5000) { // 每 5 秒发一次测试数据
    lastMsg = now;
    
    // 向 STM32 发送测试数据 (原有的测试逻辑保留)
    Serial2.println("Hello STM32");
    
    // 也可以向树莓派发布一条在线状态
    client.publish("esp32/status", "ESP32 在线");
  }
}