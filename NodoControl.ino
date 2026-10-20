/*
 * ============================================================
 *  NODO CONTROL - ESP32 (maestro Bluetooth)
 *  Sistema: Monitoreo Temp/Humedad con activación de ventilador
 * ============================================================
 *  - Se conecta al NodoSensor por Bluetooth
 *  - Recibe telemetría y la muestra por Serial + LEDs
 *  - Envía comandos por teclado Serial USB
 *  - Verifica ACKs del sensor
 *
 *  Hardware:
 *   - LED_FAN    → GPIO 25 (encendido si ventilador ON)
 *   - LED_ALERTA → GPIO 26 (encendido si T > umbral)
 *
 *  Comandos del monitor Serial (USB, 115200 baudios):
 *   SET <n>   → fija umbral, ej: SET 28.5
 *   ON        → enciende ventilador manual
 *   OFF       → apaga ventilador manual
 *   AUTO      → modo automático
 *   MANUAL    → modo manual
 *   STATUS    → solicita telemetría inmediata
 *   HELP      → lista de comandos
 * ============================================================
 */

#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

// ---------- LEDs ----------
#define LED_FAN     25
#define LED_ALERTA  26

// ---------- Conexión ----------
const char* NOMBRE_SENSOR = "NodoSensor_Temp";
bool conectado = false;

// ---------- Estado recibido ----------
float temperatura = 0.0, humedad = 0.0, umbral = 0.0;
int   ventilador  = 0;
String modo       = "?";

// ---------- Buffers ----------
String bufferUSB = "";
String bufferBT  = "";

// ---------- Parser simple de JSON ----------
float extraerFloat(const String& s, const String& clave) {
  int i = s.indexOf("\"" + clave + "\":");
  if (i < 0) return -999;
  int ini = i + clave.length() + 3;
  int fin = ini;
  while (fin < (int)s.length() && s[fin] != ',' && s[fin] != '}') fin++;
  return s.substring(ini, fin).toFloat();
}

String extraerString(const String& s, const String& clave) {
  int i = s.indexOf("\"" + clave + "\":\"");
  if (i < 0) return "";
  int ini = i + clave.length() + 4;
  int fin = s.indexOf('"', ini);
  return s.substring(ini, fin);
}

// ---------- Procesamiento de telemetría ----------
void procesarTelemetria(const String& msg) {
  if (msg.indexOf("\"T\":") < 0) return;   // no es telemetría
  temperatura = extraerFloat(msg, "T");
  humedad     = extraerFloat(msg, "H");
  umbral      = extraerFloat(msg, "U");
  ventilador  = (int)extraerFloat(msg, "V");
  modo        = extraerString(msg, "M");

  Serial.println("=========== TELEMETRÍA ===========");
  Serial.print (" Temperatura : "); Serial.print(temperatura, 1); Serial.println(" C");
  Serial.print (" Humedad     : "); Serial.print(humedad, 1);     Serial.println(" %");
  Serial.print (" Umbral      : "); Serial.print(umbral, 1);      Serial.println(" C");
  Serial.print (" Ventilador  : "); Serial.println(ventilador ? "ON" : "OFF");
  Serial.print (" Modo        : "); Serial.println(modo == "A" ? "AUTOMATICO" : "MANUAL");
  Serial.println("==================================");

  digitalWrite(LED_FAN,    ventilador ? HIGH : LOW);
  digitalWrite(LED_ALERTA, (temperatura > umbral) ? HIGH : LOW);
}

// ---------- Procesamiento de ACK ----------
void procesarACK(const String& msg) {
  String cmd = extraerString(msg, "ACK");
  int ok = (int)extraerFloat(msg, "OK");
  Serial.print("ACK [");
  Serial.print(cmd);
  Serial.print("] -> ");
  Serial.println(ok ? "ACEPTADO" : "RECHAZADO");
}

// ---------- Enviar comando al sensor ----------
void enviarComando(const String& cmd) {
  SerialBT.println(cmd);
  Serial.println("TX -> " + cmd);
}

// ---------- Interpretar entrada del usuario por USB ----------
void interpretarEntradaUsuario(String linea) {
  linea.trim();
  if (linea.length() == 0) return;

  String upper = linea;
  upper.toUpperCase();

  if (upper.startsWith("SET ")) {
    float v = upper.substring(4).toFloat();
    enviarComando("{\"CMD\":\"SET_THRESHOLD\",\"VAL\":" + String(v, 1) + "}");
  }
  else if (upper == "ON")     enviarComando("{\"CMD\":\"FAN_ON\"}");
  else if (upper == "OFF")    enviarComando("{\"CMD\":\"FAN_OFF\"}");
  else if (upper == "AUTO")   enviarComando("{\"CMD\":\"MODE\",\"VAL\":\"A\"}");
  else if (upper == "MANUAL") enviarComando("{\"CMD\":\"MODE\",\"VAL\":\"M\"}");
  else if (upper == "STATUS") enviarComando("{\"CMD\":\"STATUS\"}");
  else if (upper == "HELP") {
    Serial.println("--- COMANDOS DISPONIBLES ---");
    Serial.println("  SET <n>    fija umbral, ej: SET 28.5");
    Serial.println("  ON         enciende ventilador");
    Serial.println("  OFF        apaga ventilador");
    Serial.println("  AUTO       modo automatico");
    Serial.println("  MANUAL     modo manual");
    Serial.println("  STATUS     pide telemetria ahora");
    Serial.println("  HELP       esta ayuda");
  }
  else Serial.println("Comando no reconocido. Escribe HELP.");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  pinMode(LED_FAN, OUTPUT);
  pinMode(LED_ALERTA, OUTPUT);
  digitalWrite(LED_FAN, LOW);
  digitalWrite(LED_ALERTA, LOW);

  Serial.println("=== NODO CONTROL iniciando ===");
  SerialBT.begin("NodoControl_Temp", true);   // true = maestro

  Serial.print("Buscando sensor '");
  Serial.print(NOMBRE_SENSOR);
  Serial.println("'...");

  if (SerialBT.connect(NOMBRE_SENSOR)) {
    conectado = true;
    Serial.println("Conectado al sensor.");
  } else {
    Serial.println("No se pudo conectar. Reintentando en loop...");
  }

  Serial.println("Escribe HELP para ver comandos.");
}

// ---------- Loop ----------
void loop() {
  // Reconexión automática
  if (!conectado) {
    if (SerialBT.connect(NOMBRE_SENSOR)) {
      conectado = true;
      Serial.println("Reconectado.");
    } else {
      delay(2000);
      return;
    }
  }

  // 1) Datos que llegan del sensor
  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\n' || c == '\r') {
      if (bufferBT.length() > 0) {
        if (bufferBT.indexOf("\"ACK\":") >= 0) procesarACK(bufferBT);
        else                                    procesarTelemetria(bufferBT);
        bufferBT = "";
      }
    } else if (bufferBT.length() < 200) bufferBT += c;
  }

  // 2) Comandos escritos por el usuario en el monitor Serial
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (bufferUSB.length() > 0) {
        interpretarEntradaUsuario(bufferUSB);
        bufferUSB = "";
      }
    } else if (bufferUSB.length() < 60) bufferUSB += c;
  }
}
