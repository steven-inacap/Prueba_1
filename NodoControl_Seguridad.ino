
/*
 * ============================================================
 *  NODO CONTROL - ESP32 (maestro Bluetooth)  (VERSIÓN SEGURA)
 * ============================================================
 *  Medidas aplicadas — referenciadas SEC-Cxx
 * ============================================================
 */

#include <Arduino.h>
#include "BluetoothSerial.h"
#include <math.h>
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED) || \
    !defined(CONFIG_BT_SPP_ENABLED)
#error "Se requiere Bluetooth Classic SPP en un ESP32 compatible."
#endif

BluetoothSerial SerialBT;

#define LED_FAN     25
#define LED_ALERTA  26

const char* NOMBRE_SENSOR = "Nodo_Seguridad";
const char* SHARED_TOKEN  = "OT-7f3a9c2e";   // SEC-C2: mismo token que el sensor

// SEC-C3/C4: límites de telemetría válidos
static const float T_MIN = -20.0f, T_MAX = 80.0f;
static const float H_MIN =   0.0f, H_MAX = 100.0f;
static const size_t RX_MAX = 160;
static const uint32_t ACK_TIMEOUT_MS = 2000; // SEC-C9

bool conectado = false;
float temperatura = 0.0f, humedad = 0.0f, umbral = 0.0f;
int   ventilador = 0;
String modo = "?";

String bufferUSB = "";
String bufferBT  = "";

// SEC-C9: control de ACK pendiente
uint32_t ultimoEnvioComando = 0;
String   comandoPendiente   = "";

// ---------- Parsers estrictos (SEC-C3) ----------
bool extraerFloatSafe(const String& s, const String& clave, float& out) {
  int i = s.indexOf("\"" + clave + "\":");
  if (i < 0) return false;

  int ini = i + clave.length() + 3;
  int fin = ini;

  while (fin < (int)s.length() && s[fin] != ',' && s[fin] != '}') fin++;

  String v = s.substring(ini, fin);
  v.trim();

  if (v.length() == 0 || v.length() > 12) return false;

  bool digito = false;
  bool punto = false;
  int inicio = (v[0] == '-' || v[0] == '+') ? 1 : 0;

  if (inicio == (int)v.length()) return false;

  for (int k = inicio; k < (int)v.length(); ++k) {
    char c = v[k];

    if (isdigit((unsigned char)c)) {
      digito = true;
    } else if (c == '.' && !punto) {
      punto = true;
    } else {
      return false;
    }
  }

  if (!digito) return false;

  out = v.toFloat();
  return !isnan(out) && isfinite(out);
}

String extraerStringSafe(const String& s, const String& clave) {
  int i = s.indexOf("\"" + clave + "\":\"");
  if (i < 0) return "";

  int ini = i + clave.length() + 4;
  int fin = s.indexOf('"', ini);

  if (fin < 0) return "";

  return s.substring(ini, fin);
}

// ---------- Procesamiento de telemetría ----------
void procesarTelemetria(const String& msg) {
  if (msg.indexOf("\"T\":") < 0) return;

  float t, h, u, v;

  if (!extraerFloatSafe(msg, "T", t) ||
      !extraerFloatSafe(msg, "H", h) ||
      !extraerFloatSafe(msg, "U", u) ||
      !extraerFloatSafe(msg, "V", v)) {
    Serial.println("[SEC] telemetría inválida: descartada");
    return;
  }

  // SEC-C4: validación de rangos
  if (t < T_MIN || t > T_MAX ||
      h < H_MIN || h > H_MAX ||
      u < 10.0f || u > 60.0f ||
      (v != 0.0f && v != 1.0f)) {
    Serial.println("[SEC] telemetría fuera de rango: descartada");
    return;
  }

  String nuevoModo = extraerStringSafe(msg, "M");

  if (nuevoModo != "A" && nuevoModo != "M") {
    Serial.println("[SEC] modo inválido: telemetría descartada");
    return;
  }

  temperatura = t;
  humedad = h;
  umbral = u;
  ventilador = (int)v;
  modo = nuevoModo;

  Serial.println("=========== TELEMETRÍA ===========");
  Serial.printf(" Temperatura : %.1f C\n Humedad     : %.1f %%\n",
                temperatura, humedad);
  Serial.printf(" Umbral      : %.1f C\n Ventilador  : %s\n",
                umbral, ventilador ? "ON" : "OFF");
  Serial.printf(" Modo        : %s\n",
                modo == "A" ? "AUTOMATICO" : "MANUAL");
  Serial.println("==================================");

  digitalWrite(LED_FAN, ventilador ? HIGH : LOW);
  digitalWrite(LED_ALERTA, (temperatura > umbral) ? HIGH : LOW);
}

void procesarACK(const String& msg) {
  String cmd = extraerStringSafe(msg, "ACK");
  float okf;

  if (cmd.length() == 0 ||
      !extraerFloatSafe(msg, "OK", okf) ||
      (okf != 0.0f && okf != 1.0f)) {
    Serial.println("[SEC] ACK inválido: descartado");
    return;
  }

  int ok = (int)okf;

  Serial.printf("ACK [%s] -> %s\n",
                cmd.c_str(), ok ? "ACEPTADO" : "RECHAZADO");

  // SEC-C9: limpiar pendiente si coincide
  if (cmd == comandoPendiente) comandoPendiente = "";
}

// ---------- Envío con token (SEC-C2) ----------
void enviarComando(const String& payload) {
  if (!conectado || !SerialBT.connected()) {
    Serial.println("[BT] Sin conexión: comando no enviado.");
    return;
  }

  // SEC-C9: no sustituir un ACK pendiente por otro comando
  if (comandoPendiente.length() > 0) {
    Serial.println("[SEC] Esperando ACK del comando anterior.");
    return;
  }

  // Inyecta el token en el payload JSON
  String conToken = payload;
  int cierre = conToken.lastIndexOf('}');
  if (cierre < 0) return;

  conToken = conToken.substring(0, cierre) +
             ",\"TK\":\"" + SHARED_TOKEN + "\"}";

  int c = payload.indexOf("\"CMD\":\"");
  if (c < 0) return;

  int ini = c + 7;
  int f = payload.indexOf('"', ini);
  if (f < 0) return;

  String nuevoComando = payload.substring(ini, f);

  size_t enviados = SerialBT.println(conToken);
  if (enviados == 0) {
    Serial.println("[BT] Error al enviar comando.");
    return;
  }

  Serial.println("TX -> " + conToken);

  ultimoEnvioComando = millis();
  // Guarda el nombre de comando para timeout de ACK
  comandoPendiente = nuevoComando;
}

// ---------- Entrada de usuario (SEC-C6) ----------
void interpretarEntradaUsuario(String linea) {
  linea.trim();

  if (linea.length() == 0 || linea.length() > 32) return; // SEC-C10

  String upper = linea;
  upper.toUpperCase();

  if (upper.startsWith("SET ")) {
    float v;

    // SEC-C10: validación estricta del argumento
    if (!extraerFloatSafe("{\"V\":" + upper.substring(4) + "}", "V", v) ||
        v < 10.0f || v > 60.0f) {
      Serial.println("[SEC] SET inválido (rango 10-60)");
      return;
    }

    enviarComando("{\"CMD\":\"SET_THRESHOLD\",\"VAL\":" +
                  String(v, 1) + "}");
  }
  else if (upper == "ON")     enviarComando("{\"CMD\":\"FAN_ON\"}");
  else if (upper == "OFF")    enviarComando("{\"CMD\":\"FAN_OFF\"}");
  else if (upper == "AUTO")   enviarComando("{\"CMD\":\"MODE\",\"VAL\":\"A\"}");
  else if (upper == "MANUAL") enviarComando("{\"CMD\":\"MODE\",\"VAL\":\"M\"}");
  else if (upper == "STATUS") enviarComando("{\"CMD\":\"STATUS\"}");
  else if (upper == "HELP") {
    Serial.println("SET <n> | ON | OFF | AUTO | MANUAL | STATUS | HELP");
  } else {
    Serial.println("Comando no reconocido.");
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);

  pinMode(LED_FAN, OUTPUT);
  pinMode(LED_ALERTA, OUTPUT);
  digitalWrite(LED_FAN, LOW);
  digitalWrite(LED_ALERTA, LOW);

  // SEC-C1: PIN de emparejamiento para verificar el peer
  SerialBT.setPin("472913", 6);

  if (!SerialBT.begin("NodoControl_Temp", true)) {
    Serial.println("[BT] Error al iniciar el maestro.");
    return;
  }

  // El nombre remoto debe coincidir con el del sensor.
  Serial.printf("Buscando sensor '%s'...\n", NOMBRE_SENSOR);

  if (SerialBT.connect(String(NOMBRE_SENSOR))) {
    conectado = true;
    Serial.println("[SEC] Conectado al sensor verificado.");
  } else {
    Serial.println("No conectado; reintento en loop.");
  }
}

// ---------- Loop ----------
void loop() {
  // SEC-C5: reconexión NO bloqueante (millis)
  static uint32_t ultimoIntento = 0;

  if (conectado && !SerialBT.connected()) {
    conectado = false;
    comandoPendiente = "";
    bufferBT = "";
    Serial.println("[BT] Conexión perdida.");
  }

  if (!conectado) {
    if (millis() - ultimoIntento > 2000) {
      ultimoIntento = millis();

      if (SerialBT.connect(String(NOMBRE_SENSOR))) {
        conectado = true;
        bufferBT = "";
        Serial.println("[SEC] Reconectado.");
      }
    }
  }

  // SEC-C9: timeout de ACK
  if (comandoPendiente.length() > 0 &&
      millis() - ultimoEnvioComando > ACK_TIMEOUT_MS) {
    Serial.printf("[SEC] ACK perdido para %s\n",
                  comandoPendiente.c_str());
    comandoPendiente = "";
  }

  // Datos entrantes del sensor
  while (SerialBT.available()) {
    char c = (char)SerialBT.read();

    if (c == '\n' || c == '\r') {
      if (bufferBT.length() > 0) {
        if (bufferBT.indexOf("\"ACK\":") >= 0) {
          procesarACK(bufferBT);
        } else {
          procesarTelemetria(bufferBT);
        }
        bufferBT = "";
      }
    } else if (bufferBT.length() < RX_MAX) {   // SEC-C7
      bufferBT += c;
    } else {
      Serial.println("[SEC] BT RX overflow");
      bufferBT = "";
    }
  }

  // Comandos del usuario por USB
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {
      if (bufferUSB.length() > 0) {
        interpretarEntradaUsuario(bufferUSB);
        bufferUSB = "";
      }
    } else if (bufferUSB.length() < 32) {      // SEC-C10
      bufferUSB += c;
    } else {
      bufferUSB = "";   // descarta línea demasiado larga
    }
  }
}
