# WirelessLab32 para ESP32-C6

Esta variante adapta el firmware al SoC ESP32-C6 y al core Arduino-ESP32 3.x.

## Requisitos

```text
Board: ESP32C6 Dev Module
Arduino-ESP32: 3.3.8 o posterior
Flash Size: 4 MB
Partition Scheme: Huge APP
USB CDC On Boot: Enabled
Upload Speed: 460800
Monitor Speed: 115200
```

## Diferencias frente al ESP32 clásico

- El ESP32-C6 usa una CPU RISC-V.
- Ofrece Wi-Fi 6 en 2.4 GHz, Bluetooth LE y 802.15.4.
- No ofrece Bluetooth Classic.
- `BLEScan::start()` devuelve un puntero en Arduino-ESP32 3.x.
- `BLEAdvertisedDevice::getManufacturerData()` devuelve `String` en 3.x.
- El sketch verifica `CONFIG_IDF_TARGET_ESP32C6` durante la compilación.

Las funciones del proyecto usan BLE, no Bluetooth Classic, por lo que el escaneo,
advertising y las demostraciones GATT se conservan. El beacon lab utiliza
`esp_wifi_80211_tx()`, API disponible en el ESP32-C6 cuando la interfaz Wi-Fi está
inicializada.

## Estado de validación

La adaptación fue compilada correctamente con Arduino-ESP32 3.3.8, board
`ESP32C6 Dev Module`, Huge APP y USB CDC habilitado:

```text
Programa: 1,425,944 bytes (45%)
RAM global: 45,296 bytes (13%)
```

Queda pendiente la carga y prueba física en la placa.
