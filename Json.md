## **协议规范（JSON）**  
### **1. 设备上报数据**  
```json
{
  "command": "upload",
  "device_id": "sensor_001",
  "data": {
    "temperature": 25.6,
    "soil_moisture": 43.2,
    "temp_threshold": 30.0,
    "moisture_threshold": 40.0,
    "watering": false
  }
}
```
### **2. 监控端获取数据**  
```json
{
  "command": "get_data",
  "device_id": "sensor_001"
}
```
**服务器返回**  
```json
{
  "command": "data_response",
  "device_id": "sensor_001",
  "data": {
    "temperature": 25.6,
    "soil_moisture": 43.2,
    "temp_threshold": 30.0,
    "moisture_threshold": 40.0,
    "watering": false
  }
}
```

