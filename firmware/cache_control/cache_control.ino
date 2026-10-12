#include <WiFi.h>
#include <PubSubClient.h>
#include <SPIFFS.h> 
#include <queue> // 【引入队列库】

// --- WiFi 配置 ---
const char* ssid = "YOUR_WIFI_SSID";      
const char* password = "YOUR_WIFI_PASSWORD"; 

// --- MQTT 配置 ---
const char* mqtt_server = "YOUR_PI_IP"; 
const int mqtt_port = 1883;
const char* mqtt_topic_pub = "home/fan/status"; 
const char* mqtt_topic_sub = "pi/to/esp32";     

WiFiClient espClient;
PubSubClient client(espClient);
HardwareSerial SerialSTM(2); 
String receivedData = "";

// 【新增】数据队列，用于缓存未发送的实时数据
std::queue<String> dataQueue; 
bool isSyncing = false; 
unsigned long lastReconnectAttempt = 0;

void callback(char* topic, byte* payload, unsigned int length) {
  String cmd;
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.print("Received Command: "); Serial.println(cmd);
  SerialSTM.println(cmd); 
}

void setup() {
  Serial.begin(115200); 
  SerialSTM.setRxBufferSize(1024); 
  SerialSTM.begin(9600, SERIAL_8N1, 16, 17); 

  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS Mount Failed! Formatting...");
    SPIFFS.format();
    SPIFFS.begin(true);
  }
  Serial.println("SPIFFS Mount Success.");

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); 
}

void setup_wifi() {
  delay(10);
  Serial.print("Connecting to "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected. IP: "); Serial.println(WiFi.localIP());
}

void attemptReconnect() {
  unsigned long now = millis();
  if (now - lastReconnectAttempt > 5000) { 
    lastReconnectAttempt = now;
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      client.subscribe(mqtt_topic_sub);
      Serial.print("Subscribed to: "); Serial.println(mqtt_topic_sub);
    } else {
      Serial.print("failed, rc="); Serial.println(client.state());
    }
  }
}

void loop() {
  // 1. 处理网络状态
  if (!client.connected()) {
    attemptReconnect();
  } else {
    client.loop();
  }

  // 2. 【最优先】读取串口数据并放入队列（绝对不阻塞）
  while (SerialSTM.available()) {
    char c = SerialSTM.read();
    if (c == '\n') { 
      if (receivedData.length() > 0) {
        // 不管网络通不通，先存入队列
        dataQueue.push(receivedData);
        // 【修复点】打印出来，让你知道数据确实收到了！
        Serial.println("收到实时数据: " + receivedData); 
        receivedData = ""; 
      }
    } else if (c != '\r') {
      receivedData += c; 
    }
  }

  // 3. 处理队列中的数据（发送 或 缓存）
  if (!dataQueue.empty()) {
    if (WiFi.status() == WL_CONNECTED && client.connected()) {
      // 网络正常，从队列头部取一条发送
      String data = dataQueue.front();
      dataQueue.pop();
      client.publish(mqtt_topic_pub, data.c_str());
      // 注意：这里不打印，避免刷屏，上面第2步已经打印过“收到实时数据”了
    } else {
      // 网络断开，把队列里积压的数据全部写入SPIFFS
      Serial.println("[断网缓存] 队列积压中...");
      File file = SPIFFS.open("/offline.txt", FILE_APPEND);
      if (file) {
        while (!dataQueue.empty()) {
          String data = dataQueue.front();
          dataQueue.pop();
          file.println(data);
        }
        file.close();
      }
    }
  }

  // 4. 处理离线数据补发
  if (WiFi.status() == WL_CONNECTED && client.connected() && !isSyncing) {
    if (SPIFFS.exists("/offline.txt")) {
      isSyncing = true; 
      Serial.println("发现离线数据，开始补发...");
      
      File file = SPIFFS.open("/offline.txt", FILE_READ);
      if (file) {
        while (file.available()) {
          String line = file.readStringUntil('\n');
          line.trim();
          if(line.length() > 0) {
            client.publish(mqtt_topic_pub, line.c_str());
            delay(30); 
          }
        }
        file.close(); 
        delay(200);   
        SPIFFS.remove("/offline.txt"); 
        Serial.println("离线数据补发完成，文件已删除。");
      }
      isSyncing = false; 
    }
  }
}