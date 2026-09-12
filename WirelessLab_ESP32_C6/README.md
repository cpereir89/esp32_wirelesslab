# WirelessLab32 para ESP32-C6

Esta variante usa Wi-Fi 6, BLE e IEEE 802.15.4 del ESP32-C6. El módulo Zigbee
funciona exclusivamente como End Device para una red administrada por Home
Assistant ZHA y un coordinador CC2652P.

## Requisitos de compilación

```text
Board: ESP32C6 Dev Module
Arduino-ESP32: 3.3.8 o posterior
Flash Size: 4 MB
Partition Scheme: Custom
Zigbee Mode: Zigbee ED (end device)
USB CDC On Boot: Disabled
Upload Speed: 460800
Monitor Speed: 115200
```

El archivo `partitions.csv` reserva una aplicación grande y replica las
particiones persistentes `zb_storage` y `zb_fct` de la tabla Zigbee End Device
oficial. No ofrece OTA.

Compilación verificada con Arduino-ESP32 3.3.8:

```text
Programa: 1,742,088 bytes
RAM global: 60,736 bytes (18%)
```

## Arquitectura Zigbee

```text
Home Assistant VM
└── ZHA
    └── CC2652P USB coordinator
        └── ESP32-C6 Zigbee End Device
            ├── Light profile
            └── Sensor profile

CC2531 USB dongle
└── Passive sniffer
    └── Wireshark
```

El CC2652P es el único coordinador. El ESP32-C6 no forma una red, no abre
permit-join y no actúa como Trust Center. El CC2531 solo observa tráfico.

## Joining con ZHA

Después de cada reinicio el perfil seleccionado vuelve a `SENSOR`. Selecciónalo
antes de iniciar Zigbee:

```text
zigbee profile light
```

o:

```text
zigbee profile sensor
```

En Home Assistant abre:

```text
Settings
→ Devices & services
→ Zigbee Home Automation
→ Add device
```

Luego ejecuta:

```text
zigbee join
```

El firmware inicia `ZIGBEE_END_DEVICE` y realiza network steering sobre los
canales Zigbee disponibles. `Zigbee.begin()` solo confirma el arranque de la
pila; el estado `Joined: yes` se establece después de que
`Zigbee.connected()` y `esp_zb_bdb_dev_joined()` confirman la asociación.

## Perfil LIGHT

El endpoint 10 usa `ZigbeeLight` y los clusters estándar Basic, Identify y
On/Off. ZHA debe crear una entidad Light:

```text
zigbee profile light
zigbee join
zigbee light on
zigbee light off
zigbee light toggle
zigbee light status
```

Manufacturer: `WirelessLab32`

Model: `ESP32-C6 Lab Light`

Los cambios desde Home Assistant actualizan el LED y se imprimen como
`source=remote`. La constante `ZIGBEE_LIGHT_LED_ACTIVE_LOW` permite invertir su
polaridad. Si `LED_BUILTIN` no está definido, el pin fallback editable es GPIO
8.

## Perfil SENSOR

El endpoint 10 usa `ZigbeeTempSensor`, dispositivo estándar Temperature Sensor
con cluster Temperature Measurement `0x0402` y atributo MeasuredValue `0x0000`.

Manufacturer: `WirelessLab32`

Model: `ESP32-C6 Lab Sensor`

```text
zigbee profile sensor
zigbee sensor value 23.50
zigbee join
zigbee sensor send
zigbee sensor interval 2000
zigbee sensor start 30
zigbee sensor status
zigbee sensor stop
```

Límites:

- Temperatura: -40.00 a 125.00 °C.
- Intervalo: 500 a 60000 ms.
- Valor inicial: 23.50 °C.
- Intervalo inicial: 5000 ms.
- Duración máxima de reporting automático: 60 segundos.

## Leave y cambio de perfil

```text
zigbee leave
```

Esta orden usa el procedimiento local BDB oficial para abandonar la red y
limpiar la asociación. Arduino-ESP32 puede reiniciar automáticamente al recibir
la señal de leave.

Para borrar el estado Zigbee sin borrar Wi-Fi o BLE:

```text
zigbee factory-reset
confirm zigbee factory-reset
reboot
```

La confirmación expira después de 15 segundos. Si la pila está activa se usa
`Zigbee.factoryReset(false)`; si todavía no inició, se borra únicamente la
partición `zb_storage`. El firmware nunca imprime Network Keys, install codes ni
otros secretos Zigbee.

## Convivencia de radios

Antes de iniciar Zigbee se detienen Wi-Fi y BLE. Mientras la pila Zigbee esté
activa, esos comandos se bloquean. Para regresar a Wi-Fi o BLE se debe abandonar
la red y reiniciar:

```text
zigbee leave
```

o, después de un factory reset:

```text
reboot
```
