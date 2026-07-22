# ESP32 WirelessLab

WirelessLab32 es un firmware educativo para estudiar conceptos de seguridad
inalámbrica en un ESP32 clásico desde una interfaz de comandos por Serial.

> Utilízalo exclusivamente en equipos propios y laboratorios autorizados. Las
> demostraciones incluyen límites de duración y datos ficticios. No introduzcas
> credenciales reales en el portal de entrenamiento.

## Hardware y entorno probado

- ESP32-D0WD-V3 rev. 3.1.
- Flash de 4 MB.
- Arduino IDE: `ESP32 Dev Module`.
- Arduino-ESP32 2.0.17 y API BLE clásica (`BLEDevice.h`).
- Monitor Serial a 115200 baudios.
- Partition Scheme: `Huge APP`.
- Upload Speed: 460800 o menor.

PlatformIO está configurado con `espressif32@6.10.0`, `huge_app.csv` y upload a
460800 para reproducir el entorno usado durante el desarrollo.

## Funciones

### Wi-Fi

- Escaneo de access points y listado de SSID, BSSID, canal y RSSI.
- Beacon/SSID lab con un máximo de cinco nombres y 60 segundos.
- Presets de SSID editables mediante `WIFI_SPAM_SSIDS`.
- Canal configurable entre 1 y 13.
- Portal cautivo de entrenamiento con DNS wildcard y logo SVG inline.
- Captura temporal por Serial del formulario del portal; no se escribe en flash
  ni SD.

### Bluetooth Low Energy

- Escaneo activo y almacenamiento de hasta 100 dispositivos únicos.
- Listado de dirección, RSSI, nombre, UUID y manufacturer data.
- Advertising estable con nombre fijo `WirelessLab32`.
- Lab BLE limitado a 60 segundos con presets ficticios y rotación de mensajes
  GATT para clientes que habilitaron notificaciones.
- Simulación BlueSnarf: tres contactos ficticios en formato vCard publicados
  como características GATT de lectura.
- Simulación Bluejacking opt-in mediante notificaciones GATT.
- Control BLE exclusivamente por Serial; no existe una interfaz web BLE.

El firmware no contiene payloads Apple Continuity, Fast Pair, AirPods ni otros
paquetes diseñados para provocar pop-ups no solicitados.

## Compilación con Arduino IDE

1. Instala el core Arduino-ESP32 2.0.17.
2. Abre `WirelessLab32/WirelessLab32.ino`.
3. Configura:

   ```text
   Board: ESP32 Dev Module
   Flash Size: 4 MB
   Partition Scheme: Huge APP
   Upload Speed: 460800
   Monitor Speed: 115200
   ```

4. Compila y carga el sketch.
5. Abre el monitor Serial con final de línea `Newline`.

## Compilación con PlatformIO

```bash
pio run
pio run --target upload
pio device monitor
```

## Comandos

### Sistema

```text
help
info
status
stop
reboot
```

`stop` detiene el portal, beacon lab, escaneo y advertising activos, y regresa a
modo `IDLE`.

### Wi-Fi

```text
wifi scan
wifi list

wifi beacon add <ssid>
wifi beacon clear
wifi beacon channel <1-13>
wifi beacon start [seconds]

wifi spam load
wifi spam start [seconds]

portal start [ssid]
portal stop
```

Ejemplo:

```text
wifi beacon clear
wifi beacon add Workshop_AP
wifi beacon add Security_Lab
wifi beacon channel 6
wifi spam start 30
stop
```

Si la lista está vacía, `wifi spam start` carga automáticamente los presets de
`WIFI_SPAM_SSIDS`. La duración máxima es 60 segundos.

### BLE

```text
ble mode
ble scan
ble list
ble advertise start
ble advertise stop

ble spam load
ble spam start [seconds]
ble spam status
ble spam stop

ble bluesnarf demo
ble bluejack demo
ble bluejack send <message>
ble exit
```

## Primera prueba BLE

```text
ble mode
ble advertise start
ble advertise stop
ble scan
ble list
```

Puedes observar `WirelessLab32` con nRF Connect o LightBlue. El servicio GATT usa
el UUID:

```text
12345678-1234-1234-1234-1234567890ab
```

## BlueSnarf lab

```text
ble bluesnarf demo
```

Conecta voluntariamente desde nRF Connect, abre el servicio GATT y lee las tres
características de contactos. Todos los nombres, teléfonos y correos son ficticios.
La práctica demuestra el riesgo de publicar información sensible sin exigir
autenticación ni cifrado.

## Bluejacking lab

```text
ble bluejack demo
ble bluejack send Welcome to the classroom lab
```

El estudiante debe conectarse y habilitar notificaciones en nRF Connect. Los
mensajes admiten hasta 80 caracteres y solo se notifican a clientes suscritos.

## Portal cautivo de entrenamiento

```text
portal start WirelessLab32-Demo
```

La página advierte que es un entorno de entrenamiento y que no deben utilizarse
datos reales. El formulario se imprime únicamente en RAM y Serial; no se persiste.
Detén el portal con:

```text
portal stop
```

## Límites y diseño

- Wi-Fi beacon lab: cinco SSID y 60 segundos como máximo.
- BLE advertising lab: 60 segundos como máximo.
- Bluejacking requiere conexión y suscripción voluntaria.
- BLE y Wi-Fi comparten la radio de 2.4 GHz y se operan por modos.
- El stack BLE se inicializa una sola vez para reducir fragmentación del heap.
- El advertising estable evita datos temporales y scan responses personalizados.
- No se guardan formularios, escaneos ni contactos en almacenamiento persistente.

## Estructura

```text
.
├── WirelessLab32/
│   └── WirelessLab32.ino
├── platformio.ini
└── README.md
```
