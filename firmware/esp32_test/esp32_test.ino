#include <WiFi.h>
#include <PubSubClient.h>

// --- WiFi 配置 ---
const char* ssid = "YOUR_WIFI_SSID";      
const char* password = "YOUR_WIFI_PASSWORD"; 

// --- MQTT 配置 (树莓派IP) ---
const char* mqtt_server = "YOUR_PI_IP"; 
const int mqtt_port = 1883;
const char* mqtt_topic_pub = "home/fan/status"; // 上报数据的主题
const char* mqtt_topic_sub = "pi/to/esp32";     // 接收指令的主题

WiFiClient espClient;
PubSubClient client(espClient);

// --- 串口2，用于与STM32通信 ---
HardwareSerial SerialSTM(2); 
String receivedData = "";

// --- MQTT 回调函数：收到树莓派指令时触发 ---
void callback(char* topic, byte* payload, unsigned int length) {
  String cmd;
  for (unsigned int i = 0; i < length; i++) {
    cmd += (char)payload[i];
  }
  cmd.trim();
  
  Serial.print("Received Command from Pi: ");
  Serial.println(cmd);
  
  // 原样转发给 STM32
  SerialSTM.println(cmd); 
}

void setup() {
  Serial.begin(115200); 
  SerialSTM.begin(9600, SERIAL_8N1, 16, 17); 

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); // 注册回调函数
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
  Serial.println("\nWiFi connected. IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      // 连接成功后，立刻订阅树莓派的指令主题！
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
  client.loop(); // 保持MQTT心跳，处理接收到的消息

  // 读取STM32发来的串口数据并上报
  while (SerialSTM.available()) {
    char c = SerialSTM.read();
    if (c == '\n') { 
      if (receivedData.length() > 0) {
        Serial.print("Received from STM32: ");
        Serial.println(receivedData);
        
        // 发布给树莓派
        client.publish(mqtt_topic_pub, receivedData.c_str());
        receivedData = ""; 
      }
    } else if (c != '\r') {
      receivedData += c; 
    }
  }
}