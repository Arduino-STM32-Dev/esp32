// ============================================================
// ESP32 无线网关 —— 树莓派 <-> STM32 的 MQTT/UART 桥
//   · 订阅 MQTT  pi/to/esp32  ，把指令经 UART 转发给 STM32
//   · 读取 STM32 串口数据，发布到 MQTT  home/fan/status
// 链路：树莓派 --MQTT--> ESP32(GPIO16/17) --UART--> STM32F103
// 注意：WiFi / IP 请填入你自己的，切勿提交真实信息
// ============================================================
#include <WiFi.h>
#include <PubSubClient.h>

// --- WiFi 配置（填入你自己的）---
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// --- MQTT 配置（树莓派 IP，填入你自己的）---
const char* mqtt_server = "YOUR_RASPBERRY_PI_IP";
const int mqtt_port = 1883;
const char* mqtt_topic_pub = "home/fan/status";  // 上报数据
const char* mqtt_topic_sub = "pi/to/esp32";      // 接收指令

WiFiClient espClient;
PubSubClient client(espClient);

// --- 串口2，与 STM32 通信（RX=GPIO16, TX=GPIO17）---
HardwareSerial SerialSTM(2);
String receivedData = "";

// MQTT 回调：收到树莓派指令 -> 原样转发给 STM32
void callback(char* topic, byte* payload, unsigned int length) {
  String cmd;
  for (unsigned int i = 0; i < length; i++) {
    cmd += (char)payload[i];
  }
  cmd.trim();
  Serial.print("Received Command from Pi: ");
  Serial.println(cmd);
  SerialSTM.println(cmd);
}

void setup() {
  Serial.begin(115200);
  SerialSTM.begin(9600, SERIAL_8N1, 16, 17);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected. IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);

    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      client.subscribe(mqtt_topic_sub);
      Serial.print("Subscribed to: ");
      Serial.println(mqtt_topic_sub);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  while (SerialSTM.available()) {
    char c = SerialSTM.read();
    if (c == '\n') {
      if (receivedData.length() > 0) {
        Serial.print("Received from STM32: ");
        Serial.println(receivedData);
        client.publish(mqtt_topic_pub, receivedData.c_str());
        receivedData = "";
      }
    } else if (c != '\r') {
      receivedData += c;
    }
  }
}
