Integrante: Steven Stock

Caso 1: Ventilador

Diagrama: 

https://drive.google.com/file/d/1ULmcg1ZzhdTKfS06avHNpIzTz1EMrJTt/view?usp=sharing

Mensaje Serial Sensor:


Mensaje Serial Control:

=== NODO CONTROL iniciando ===
E (10585) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (10585) task_wdt:  - IDLE0 (CPU 0)
E (10585) task_wdt: Tasks currently running:
E (10585) task_wdt: CPU 0: btController
E (10585) task_wdt: CPU 1: IDLE1
E (10585) task_wdt: Aborting.
E (10585) task_wdt: Print CPU 0 (current core) backtrace

Backtrace: 0x4003c9fd:0x3ffd2b60 0x4005516e:0x3ffd2b80 0x40179dba:0x3ffd2ba0 0x4016428f:0x3ffd2be0 0x401647c7:0x3ffd2c00 0x40093fb1:0x3ffd2c30

ELF file SHA256: b280dcef1

Rebooting...
ets Jul 29 2019 12:21:46

rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
configsip: 0, SPIWP:0xee
clk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00
mode:DIO, clock div:2
load:0x3fff0030,len:1156
load:0x40078000,len:11456
ho 0 tail 12 room 4
load:0x40080400,len:2972
entry 0x400805dc

# 1: Brechas de Seguridad:

1. Brechas de seguridad identificadas

En el análisis de NodoSensor.ino se detectaron once brechas relevantes. La más crítica es la ausencia total de autenticación en el enlace Bluetooth: el nodo se anuncia como NodoSensor_Temp mediante SerialBT.begin("NodoSensor_Temp") en setup(), sin PIN ni emparejamiento, de modo que cualquier dispositivo dentro del radio de cobertura puede conectarse. A ello se suma que procesarComando() ejecuta cualquier comando recibido (FAN_ON, FAN_OFF, SET_THRESHOLD, MODE) sin verificar identidad ni autorización, permitiendo que un atacante accione físicamente el relé. El parser de comandos es frágil: utiliza cmd.indexOf("\"VAL\":") y substring() sin validación estricta del contenido, y toFloat() sobre cadenas arbitrarias puede producir valores corruptos o comportamientos erráticos. No existe limitación de tasa en loop(), por lo que un flood de comandos puede desgastar el relé por conmutación excesiva o agotar la CPU. Tampoco hay timeout de sesión Bluetooth, de manera que una conexión permanece abierta indefinidamente si el maestro se aleja. Los comandos destructivos se aplican sin confirmación ni registro persistente, y el logging se limita a Serial.print local sin timestamp ni trazabilidad forense. No se implementa watchdog ni estado fail-safe: si el enlace se cae, el relé mantiene el último estado, con riesgo térmico o eléctrico. Finalmente, el buffer de recepción usa String dinámico acotado a 200 bytes, lo que puede fragmentar el heap del ESP32, y el nombre del dispositivo revela su función, facilitando el reconocimiento pasivo.

En NodoControl.ino se identificaron diez brechas. La conexión se realiza con SerialBT.connect(NOMBRE_SENSOR) confiando únicamente en el nombre del peer, lo que permite suplantación del sensor por un dispositivo rogue que entregue telemetría falsa. El maestro tampoco se autentica ante el sensor, de modo que cualquier ESP32 con el nombre correcto puede establecer el enlace. Los parsers propios extraerFloat() y extraerString() usan indexOf sin validación estricta y pueden devolver valores basura como -999, que luego se muestran y se usan para decidir el estado de LED_ALERTA en procesarTelemetria(). No se validan rangos de temperatura, humedad ni umbral, por lo que una lectura corrupta produce falsa alarma o falsa calma. La reconexión usa delay(2000) bloqueante en loop(), congelando la lectura de USB y Bluetooth. Cualquier usuario con acceso al monitor Serial puede emitir ON, OFF o SET sin control de acceso en interpretarEntradaUsuario(), y toFloat() sobre la entrada del usuario acepta cadenas no numéricas que se envían como umbral inválido. El maestro envía comandos y confía en que se aplicaron, sin ACK obligatorio ni timeout de ACK, lo que produce desincronización silenciosa entre el estado mostrado y el estado real del relé. Los buffers RX, aunque acotados en longitud, no validan contenido, lo que deja abierta la inyección de payloads malformados.

2. Medidas de seguridad aplicadas en el Nodo Sensor

En NodoSensor_seguro.ino se incorporó un PIN de emparejamiento Bluetooth mediante SerialBT.setPin("472913", 6) en setup(), lo que obliga a un emparejamiento previo y reduce el reconocimiento pasivo del dispositivo. Sobre esa base se añadió un token compartido SHARED_TOKEN verificado por la función autorizado(), que compara la cadena recibida en el campo "TK" con el token esperado usando una comparación en tiempo constante aproximado (acumulando diferencias con XOR byte a byte), de modo que procesarComando() rechaza con enviarACK("UNAUTH", 0, false) cualquier comando que no lo incluya o no coincida. La validación numérica se endureció con parseFloatStrict(), que acota longitud, verifica que solo contenga dígitos, signo y punto, y descarta NaN o infinitos; se usa en el manejo de SET_THRESHOLD junto con la comprobación de rango UMBRAL_MIN y UMBRAL_MAX (10 a 60 °C). Se agregó rate limiting al inicio de procesarComando() mediante la constante CMD_MIN_INTERVAL_MS, que descarta comandos demasiado frecuentes y protege tanto la CPU como el ciclo de vida del relé. El timeout de sesión Bluetooth se implementa en loop() con BT_TIMEOUT_MS: si no hay cliente conectado durante ese intervalo, se fuerza el estado seguro del ventilador y se limpia el buffer. El logging de seguridad se centralizó en aplicarVentilador(), enviarACK() y procesarComando() con Serial.printf("[SEC][%lu] …", millis(), …), añadiendo timestamp a cada evento relevante. La robustez del firmware se refuerza con esp_task_wdt_init(15, true) y esp_task_wdt_reset() (watchdog), con digitalWrite(RELAY_PIN, LOW) en setup() como estado seguro de arranque y con el bloque fail-safe en loop() que apaga el ventilador si en modo AUTO no hay enlace durante FAILSAFE_MS. Finalmente, el buffer de recepción se acotó con RX_MAX y se descarta explícitamente al desbordarse, evitando la fragmentación de heap.

3. Medidas de seguridad aplicadas en el Nodo Control

En NodoControl_seguro.ino se replicó el PIN de emparejamiento en setup() con SerialBT.setPin("472913", 6), lo que permite verificar el peer antes de aceptar la conexión. Cada comando emitido por enviarComando() incorpora el token compartido inyectando "TK":"…" antes del cierre del JSON, de modo que el sensor pueda autenticar al maestro; además, enviarComando() registra comandoPendiente y ultimoEnvioComando para el control de ACK. Los parsers se sustituyeron por extraerFloatSafe() y extraerStringSafe(), que validan longitud y carácter a carácter y descartan NaN o infinitos; se usan en procesarTelemetria() y procesarACK(). La telemetría se valida contra los límites T_MIN, T_MAX, H_MIN, H_MAX y el rango de umbral (10 a 60 °C) antes de actualizar el estado y los LEDs, descartando mensajes fuera de rango con un mensaje [SEC]. La reconexión dejó de ser bloqueante: en loop() se usa millis() con ultimoIntento en lugar de delay(), liberando el ciclo para seguir leyendo USB y Bluetooth. La entrada del usuario se valida estrictamente en interpretarEntradaUsuario(), que acota la longitud de línea, exige que el argumento de SET sea numérico y esté dentro del rango permitido, y descarta líneas demasiado largas en el buffer USB. El ACK se hace exigible con ACK_TIMEOUT_MS: si el comando pendiente no recibe confirmación en ese plazo, loop() emite [SEC] ACK perdido y limpia el pendiente, y procesarACK() limpia el pendiente cuando coincide el nombre del comando. Los buffers BT y USB están acotados con RX_MAX y 32 caracteres respectivamente, con descarte explícito al desbordarse.

# 2: Prácticas OT

El conjunto de medidas responde al modelo de segmentación Zone & Conduit de IEC 62443 y a las recomendaciones de NIST SP 800-82 para entornos de control. El enlace Bluetooth entre ambos nodos actúa como conduit entre un dispositivo de nivel 1 (sensor/actuador) y uno de supervisión; al exigir PIN y token se cumple el requisito SR 1.1 de identificación y autenticación de dispositivos, que en OT es especialmente crítico porque los protocolos heredados suelen confiar implícitamente en la red. La verificación por comando en autorizado() implementa SR 2.1 (control de uso) y evita que un atacante accione el relé, un actuador físico cuyo mal uso tiene impacto directo en el proceso y en la seguridad de las personas. El parser estricto y la validación de rangos cubren SR 3.1 (integridad de entrada), porque en OT un payload corrupto puede dejar el proceso en un estado inseguro —por ejemplo, un umbral de 0 °C o 999 °C—, y la comprobación de rangos de telemetría en el maestro impide que lecturas basura se propaguen a la lógica de alarma. El watchdog, el estado seguro del relé al arranque y el bloque fail-safe materializan el principio de disponibilidad y estado seguro de NIST 800-82: en OT el actuador debe tener un estado seguro explícitamente definido, y un ventilador que queda encendido sin enlace es un riesgo térmico y eléctrico. El rate limiting cumple SR 7.1 (protección contra DoS) y además protege la vida útil del relé frente a conmutaciones excesivas, un aspecto específico de OT que no suele considerarse en sistemas IT. El logging con timestamp y prefijo [SEC] responde a SR 6.1 (auditoría), requisito indispensable para respuesta a incidentes y análisis forense en planta. Los timeouts de sesión y de ACK implementan SR 3.2 y SR 7.x: los protocolos OT deben detectar la pérdida de enlace y transicionar a un estado seguro, no permanecer en el último estado conocido, y el maestro no debe asumir que un comando se aplicó sin confirmación. La validación estricta de la entrada USB refleja el principio de mínimo privilegio (SR 1.3) aplicado a la interfaz de operador, evitando que valores fuera de rango o payloads arbitrarios lleguen al actuador. En su conjunto, la combinación de PIN, token, parser estricto, rangos, timeouts, watchdog y logging constituye defensa en profundidad: ninguna capa basta por sí sola, pero su superposición tolera el fallo de una de ellas, que es el enfoque recomendado por IEC 62443 para sistemas ciberfísicos. Complementariamente, el uso de un nombre de dispositivo genérico y la no exposición del PIN en logs reducen el reconocimiento pasivo, alineándose con las guías de hardening de dispositivos de borde OT. Como mejoras adicionales, fuera del alcance de los archivos pero coherentes con el mismo marco, se recomienda almacenar el token en NVS cifrado con Secure Boot y Flash Encryption, firmar la telemetría con HMAC-SHA256 en lugar de un token estático, habilitar OTA firmado con rollback, centralizar los logs en un SIEM o syslog en nivel 3 de Purdue y definir un modo mantenimiento con confirmación por pulsador físico para los comandos de accionamiento del ventilador.
