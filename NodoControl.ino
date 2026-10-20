/*
 * NODO CONTROL - ESP32
 * - Se conecta al NodoSensor por Bluetooth
 * - Recibe telemetría y la muestra por Serial + LEDs
 * - Envía comandos por teclado Serial (SET_THRESHOLD, FAN_ON, FAN_OFF, MODE, STATUS)
 * - Verifica ACKs
 */

#include "BluetoothSerial.h"

BluetoothSerial SerialBT;

// ---------- LEDs indicadores ----------
#define LED_FAN     25   // Encendido si ventilador ON
#define LED_ALERTA  26   // Parpadea si T > umbral

// ---------- Conexión ----------
const char* NOMBRE_SENSOR = "NodoSensor_Temp";
bool conectado = false;

// ---------- Estado recibido ----------
float temperatura = 0.0, humedad = 0.0, umbral = 0.0;
int   ventilador  = 0;
String modo       = "?";

// ---------- Buffer Serial y BT ----------
String bufferUSB = "";
String bufferBT  = "";

// ---------- Utilidades ----------
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

// Parseo de la telemetría
void procesarTelemetria(const String& msg) {
  if (msg.indexOf("\"T\":") < 0) return;   // no es telemetría
  temperatura = extraerFloat(msg, "T");
  humedad     = extraerFloat(msg, "H");
  umbral      = extraerFloat(msg, "U");
  ventilador  = (int)extraerFloat(msg, "V");
  modo        = extraerString(msg, "M");

  // Mostrar en Serial
  Serial.println("──── TELEMETRÍA ────");
  Serial.print ("Temperatura : "); Serial.print(temperatura, 1); Serial.println(" °C");
  Serial.print ("Humedad     : "); Serial.print(humedad, 1);     Serial.println(" %");
  Serial.print ("Umbral      : "); Serial.print(umbral, 1);      Serial.println(" °C");
  Serial.print ("Ventilador  : "); Serial.println(ventilador ? "ON" : "OFF");
  Serial.print ("Modo        : "); Serial.println(modo == "A" ? "AUTOMÁTICO" : "MANUAL");
  Serial.println("────────────────────");

  // LEDs
  digitalWrite(LED_FAN, ventilador ? HIGH : LOW);
  if (temperatura > umbral) {
    digitalWrite(LED_ALERTA, HIGH);
  } else {
    digitalWrite(LED_ALERTA, LOW);
  }
}

// Procesar ACKs
void procesarACK(const String& msg) {
  String cmd = extraerString(msg, "ACK");
  int ok = (int)extraerFloat(msg, "OK");
  Serial.print("ACK [");
  Serial.print(cmd);
  Serial.print("] → ");
  Serial.println(ok ? "ACEPTADO ✓" : "RECHAZADO ✗");
}

// Enviar comando al sensor
void enviarComando(const String& cmd) {
  SerialBT.println(cmd);
  Serial.println("TX → " + cmd);
}

// Interpretar lo que el usuario teclea en el monitor Serial
void interpretarEntradaUsuario(String linea) {
  linea.trim();
  if (linea.length() == 0) return;

  linea.toUpperCase();

  if (linea.startsWith("SET ")) {
    float v = linea.substring(4).toFloat();
    enviarComando("{\"CMD\":\"SET_THRESHOLD\",\"VAL\":" + String(v, 1) + "}");
  }
  else if (linea == "ON")    enviarComando("{\"CMD\":\"FAN_ON\"}");
  else if (linea == "OFF")   enviarComando("{\"CMD\":\"FAN_OFF\"}");
  else if (linea == "AUTO")  enviarComando("{\"CMD\":\"MODE\",\"VAL\":\"A\"}");
  else if (linea == "MANUAL")enviarComando("{\"CMD\":\"MODE\",\"VAL\":\"M\"}");
  else if (linea == "STATUS")enviarComando("{\"CMD\":\"STATUS\"}");
  else if (linea == "HELP") {
    Serial.println("Comandos disponibles:");
    Serial.println("  SET <n>    → fija umbral (ej: SET 28.5)");
    Serial.println("  ON / OFF   → ventilador manual");
    Serial.println("  AUTO       → modo automático");
    Serial.println("  MANUAL     → modo manual");
    Serial.println("  STATUS     → solicita telemetría inmediata");
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

  Serial.println("Nodo Control iniciando...");
  SerialBT.begin("NodoControl_Temp", true);   // true = maestro

  Serial.println("Buscando sensor...");
  if (SerialBT.connect(NOMBRE_SENSOR)) {
    conectado = true;
    Serial.println("Conectado a " + String(NOMBRE_SENSOR));
  } else {
    Serial.println("No se pudo conectar. Reinicia el sensor y vuelve a intentar.");
  }

  Serial.println("Escribe HELP para ver comandos.");
}

// ---------- Loop ----------
void loop() {
  // Si se pierde conexión, intentar reconectar
  if (!conectado) {
    if (SerialBT.connect(NOMBRE_SENSOR)) {
      conectado = true;
      Serial.println("Reconectado ✓");
    } else {
      delay(2000);
      return;
    }
  }

  // 1) Leer Bluetooth del sensor
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

  // 2) Leer comandos por USB (monitor Serial)
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
