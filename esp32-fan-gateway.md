# ESP32 无线网关：MQTT ⇄ UART（配合树莓派控制 STM32 风扇）

> 本仓库（Arduino-STM32-Dev/esp32）的固件说明。
> 链路：**树莓派 → MQTT → ESP32 → UART → STM32F103**。

## 一、功能

- 连 WiFi，接入树莓派上的 mosquitto
- 订阅 pi/to/esp32，把收到的指令原样经 UART 转发给 STM32
- 读取 STM32 串口数据（T:..,H:..,F:..），发布到 home/fan/status

## 二、配置（务必填自己的，切勿提交真实 WiFi / IP）

    const char* ssid = "YOUR_WIFI_SSID";
    const char* password = "YOUR_WIFI_PASSWORD";
    const char* mqtt_server = "YOUR_RASPBERRY_PI_IP";

## 三、接线

| ESP32 | STM32 |
| :--- | :--- |
| GPIO16 (RX2) | PA9 (TX1) |
| GPIO17 (TX2) | PA10 (RX1) |
| GND | GND（务必共地） |

## 四、MQTT 主题

| 方向 | 主题 | 内容 |
| :--- | :--- | :--- |
| 订阅 | pi/to/esp32 | FAN_ON / FAN_OFF |
| 发布 | home/fan/status | T:26.1,H:32.0,F:1 |

## 五、使用

1. 填好 WiFi 与树莓派 IP，Arduino IDE 选择 ESP32 开发板烧录
2. 打开串口监视器（115200）确认 WiFi / MQTT 连接成功
3. 树莓派上订阅验证：

    mosquitto_sub -h localhost -t home/fan/status -v
