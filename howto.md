# Setup & Usage Guide

---

## 1. Hardware Setup & Wiring

### Logger Wiring Plan

| Component | Signal | ESP32 GPIO | Notes |
|----------|--------|------------|------|
| BMP280 | SDA | 21 | I2C shared |
| BMP280 | SCL | 22 | I2C shared |
| GY-521 | SDA | 21 | I2C shared |
| GY-521 | SCL | 22 | I2C shared |
| DHT11 | Digital | 04 | Digital input |
| OLED Display 128x64 | SDA | 21 | I2C shared |
| OLED Display 128x64 | SCL | 22 | I2C shared |
| Soil Sensor | AO | 34 | Analog input |
| Green LED (+) | - | 32 | 220Ω resistor |
| Red LED (+) | - | 33 | 220Ω resistor |
| Buzzer (+) | - | 27 | Active buzzer |
| Fall Reset Button | - | 25 | Active Low |

---

### Base Station Wiring Plan

| Component | ESP32 GPIO | Notes |
|----------|------------|------|
| OLED Display SDA | 21 | I2C shared |
| OLED Display SCL | 22 | I2C shared |
| Green LED | 32 | 220Ω resistor |
| Red LED | 33 | 220Ω resistor |
| Buzzer | 27 | Active buzzer |
| Fall Reset Button | 25 | Active Low |

---

## 2. Upload Firmware

- Set up ESP32 environment in **Arduino IDE**
- Connect and upload:
  - [Logger.ino](./Logger/Logger.ino) → Logger ESP32  
  - [Base.ino](./Base/Base.ino) → Base Station ESP32  

---

## 3. Open Project in VS Code

- Open the complete project folder in **VS Code**

---

## 4. Power Up Devices

- Turn on both:
  - Logger Station  
  - Base Station  

---

## 5. Open Firebase

- Go to your **Firebase Console**
- Open your **Realtime Database**

---

## 6. Run Data Uploader

- Open a terminal and run:

```
node uploader.js
```

---

## 7. Run Dashboard Server

- Open another terminal and run:

```
npx serve
```

---

## 8. Launch Dashboard

- Copy the localhost link shown in terminal  
- Paste it into your browser  

---

## ✅ Result

- The **live dashboard** will start running  
- Data will update automatically after synchronization  

---

### Working Procedure Video - https://youtu.be/JpjVT1_RMtQ?si=thXEctoR9-ag0ytO
