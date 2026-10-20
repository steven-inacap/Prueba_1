Integrante: Steven Stock
Caso 1: Ventilador
Diagrama: https://drive.google.com/file/d/1ULmcg1ZzhdTKfS06avHNpIzTz1EMrJTt/view?usp=sharing

# 1: Brechas de Seguridad:

1. Brechas de seguridad y medidas implementadas (con ubicación en el código)
Brecha 1: Falta de autenticación e identidad del dispositivo
Descripción: ESP-NOW utiliza direcciones MAC para identificar peers, sin ningún mecanismo de autenticación, emparejamiento o cifrado. Cualquier ESP32 dentro del mismo canal puede enviar comandos arbitrarios al NodoSensor.

Medida implementada: Ninguna medida de seguridad real. El código solo implementa una "validación de formato" de comandos (ver Brecha 2).

Ubicación en el código: En el setup() del NodoSensor, esp_now_add_peer(&peerInfo) simplemente registra la dirección MAC y establece encrypt = false. El código no implementa autenticación mutua ni verificación de identidad.

Correspondencia OWASP IoT Top 10: I1 (contraseñas débiles/adivinables/codificadas) 

Brecha 2: Superficie de ataque de comandos: falta de autorización y firma
Descripción: El callback OnDataRecv del NodoSensor acepta directamente cualquier cadena JSON recibida y ejecuta operaciones como FAN_ON, SET_THRESHOLD, etc., sin verificar el remitente ni la integridad del mensaje.

Medida implementada: Validación del valor del umbral. En OnDataRecv, cuando se procesa SET_THRESHOLD, el código verifica que el nuevo valor esté dentro del rango 10.0 ~ 60.0, rechazando valores fuera de rango y respondiendo con ACK con OK:0. Esto previene configuraciones accidentales o maliciosas que podrían causar un funcionamiento anómalo.

Ubicación en el código: Dentro de OnDataRecv(), en la rama if (cmd.startsWith("{\"CMD\":\"SET_THRESHOLD\"")), la condición if (nuevo >= 10.0 && nuevo <= 60.0).

Correspondencia OWASP IoT Top 10: I3 (interfaces del ecosistema inseguras: falta de filtrado de entrada/salida) 

Brecha 3: Datos transmitidos en texto plano (sin cifrado)
Descripción: Toda la telemetría y comandos se transmiten mediante esp_now_send() en texto plano, sin cifrado. La información de temperatura/humedad y las instrucciones de control pueden ser interceptadas por sniffers.

Medida implementada: Ninguna. ESP-NOW técnicamente soporta cifrado, pero el código establece explícitamente peerInfo.encrypt = false.

Ubicación en el código: En el setup() de ambos sketches, peerInfo.encrypt = false.

Correspondencia OWASP IoT Top 10: I7 (transferencia y almacenamiento de datos inseguros) 

Brecha 4: Ausencia de mecanismo de actualización segura
Descripción: El código no incluye ninguna funcionalidad de actualización de firmware OTA, ni verificación de firma de firmware, ni mecanismo anti-rollback.

Medida implementada: Ninguna. Este es un prototipo de demostración, no un producto desplegado.

Correspondencia OWASP IoT Top 10: I4 (falta de mecanismo de actualización segura) 

Brecha 5: Exposición de información de depuración
Descripción: Ambos sketches imprimen la dirección MAC, telemetría y contenido de comandos a través de Serial, lo cual no es un problema durante el desarrollo, pero en un dispositivo desplegado en producción filtraría información del sistema.

Medida implementada: Ninguna. Serial.print está presente en todo el código.

Ubicación en el código: En el setup() del NodoSensor, Serial.print("Mi MAC: "); Serial.println(WiFi.macAddress()); y múltiples Serial.println("TX: " + msg) en enviarTelemetria().

Brecha 6: Limitaciones de la gestión del ciclo de vida del dispositivo
Descripción: No hay gestión de credenciales, monitoreo de estado ni capacidad de desmantelamiento seguro. La dirección MAC está codificada en el firmware.

Medida implementada: Ninguna. Es una limitación de la etapa de prototipo.

# 2: Prácticas OT

Práctica 1: Segmentación de red y aislamiento (Zones & Conduits)
Manifestación en el código: El sistema se divide en dos nodos físicos independientes: NodoSensor y NodoControl. El sensor solo se comunica con el controlador a través de ESP-NOW (un canal inalámbrico punto a punto). La lógica de recolección de datos y la lógica de control están separadas y no hay acceso directo desde el exterior (como internet).

Por qué seguirla: El estándar IEC 62443 propone que las redes industriales se dividan en "zonas" según los requisitos de seguridad, y que la comunicación entre zonas se realice a través de "conductos" controlados . Este prototipo, aunque simplificado, refleja el principio de separación de funciones. En un sistema real, el NodoSensor debería estar en la zona de campo (PLC/sensores), el NodoControl en la zona de supervisión, y la comunicación entre ambos debería pasar por un firewall o pasarela, no directamente . El código no implementa una segmentación real (porque ESP-NOW es comunicación directa punto a punto), pero la arquitectura de dos nodos proporciona una base para una segmentación posterior.

Práctica 2: Validación de comandos y respuesta ACK (validación de entrada y confirmación)
Manifestación en el código: El NodoSensor valida el rango antes de ejecutar cualquier comando de modificación de umbral (10~60). Después de recibir un comando, el NodoControl espera el ACK del sensor (que contiene OK:1 o OK:0) para confirmar si la ejecución fue exitosa.

Por qué seguirla: En los sistemas OT, la ejecución errónea de comandos de control puede causar consecuencias físicas. NIST SP 800-82 destaca la importancia de la validación de entrada y el control de comandos en sistemas OT . La respuesta ACK proporciona un mecanismo de "comando-ejecución-confirmación", evitando que el operador asuma erróneamente que el comando fue efectivo. Aunque el código no implementa verificación criptográfica de comandos, la validación a nivel de lógica de negocio es una línea de defensa efectiva (aunque no suficiente).

Práctica 3: Operación no bloqueante y ciclos de control deterministas
Manifestación en el código: El NodoSensor utiliza millis() en lugar de delay() para lograr una recolección de datos cada 2 segundos, sin bloquear el bucle principal. Esto significa que el procesamiento de comandos y la recolección de datos no se afectan mutuamente.

Por qué seguirla: Los sistemas OT tienen requisitos estrictos de disponibilidad y determinismo. Si la recolección de datos bloqueara el bucle principal, los comandos de control podrían retrasarse, afectando la operación en tiempo real. NIST SP 800-82 señala que los sistemas OT deben equilibrar seguridad y disponibilidad, y que las medidas de seguridad no deben degradar el rendimiento en tiempo real . El patrón millis() es una práctica básica para garantizar la capacidad de respuesta del sistema en entornos de recursos limitados.

Práctica 4: Principio de mínimo privilegio (en la medida de lo posible)
Manifestación en el código: El NodoControl no tiene capacidad de control directo sobre los actuadores (el relé está en el lado del NodoSensor). El controlador solo puede enviar "solicitudes de comando"; la decisión final de ejecutar o no reside en el sensor (que puede rechazar solicitudes fuera de rango).

Por qué seguirla: El principio de mínimo privilegio es un requisito central de IEC 62443 y NIST SP 800-82 . El NodoControl, como interfaz de operación del usuario, no tiene privilegios de ejecución física directa; todas las acciones de control deben pasar por la validación del NodoSensor. Esto reduce el riesgo de operaciones maliciosas o erróneas.

Práctica 5: Monitoreo y visibilidad (en la medida de lo posible)
Manifestación en el código: Ambos nodos imprimen el estado de telemetría y la recepción de comandos a través de Serial, y el NodoControl utiliza LEDs para indicar el estado del ventilador y la alarma de umbral.

Por qué seguirla: NIST SP 800-82r4 (borrador) enfatiza la importancia del monitoreo de red y la visibilidad de activos como medidas de defensa proactivas . Aunque la salida Serial no es una "solución de monitoreo" en el sentido de producción, refleja el principio de "observabilidad del estado del sistema". En un despliegue real, esto debería reemplazarse por registros centralizados y monitoreo del tráfico de red.
