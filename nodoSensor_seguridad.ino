/*
 * ============================================================
 *  NODO SENSOR - ESP32  (VERSIÓN SEGURA)
 * ============================================================
 *  Medidas de seguridad aplicadas (referenciadas en comentarios SEC-xx)
 * ============================================================
 */

#include "BluetoothSerial.h"
#include "DHT.h"
#include <esp_task_wdt.h>

// ---------- Pines ----------
#define DHTPIN      4
#define DHTTYPE     DHT22
#define RELAY_PIN   5

// ---------- Límites de seguridad ----------
static const float  UMBRAL_MIN       = 10.0f;
static const float  UMBRAL_MAX       = 60.0f;
static const size_t RX_MAX           = 160;      // SEC-B10: buffer acotado
static const uint32_t CMD_MIN_INTERVAL_MS = 100; // SEC-B5: rate limit
static const uint32_t BT_TIMEOUT_MS  = 30000;    // SEC-B6: timeout de sesión
static const uint32_t FAILSAFE_MS    = 10000;    // SEC-B9: fail-safe ventilador

// SEC-B1: token compartido (en producción: NVS + rotación)
static const char* SHARED_TOKEN = "OT-7f3a9c2e";

BluetoothSerial SerialBT;
DHT dht(DHTPIN, DHTTYPE);

float temperatura = 0.0f;
float humedad     = 0.0f;
float umbral      = 30.0f;
bool  ventilador  = false;
bool  modoAuto    = true;

const unsigned long INTERVALO_LECTURA = 2000;
unsigned long ultimaLectura  = 0;
unsigned long ultimoComando  = 0;
unsigned long ultimaConexion = 0;

String bufferRX = "";

// ---------- Utilidades ----------
void aplicarVentilador(bool estado) {
  ventilador = estado;
  digitalWrite(RELAY_PIN, estado ? HIGH : LOW);
  // SEC-B8: log de seguridad con timestamp
  Serial.printf("[SEC][%lu] RELAY=%s\n", millis(), estado ? "ON" : "OFF");
}

void enviarTelemetria() {
  String msg = "{";
  msg += "\"T\":"   + String(temperatura, 1) + ",";
  msg += "\"H\":"   + String(humedad, 1)     + ",";
  msg += "\"V\":"   + String(ventilador ? 1 : 0) + ",";
  msg += "\"U\":"   + String(umbral, 1)      + ",";
  msg += "\"M\":\"" + String(modoAuto ? "A" : "M") + "\"";
  msg += "}";
  SerialBT.println(msg);
}

void enviarACK(const char* cmd, float val, bool ok) {
  String msg = "{\"ACK\":\"" + String(cmd) + "\",";
  msg += "\"VAL\":" + String(val, 1) + ",";
  msg += "\"OK\":" + String(ok ? 1 : 0) + "}";
  SerialBT.println(msg);
  Serial.printf("[SEC] ACK %s ok=%d\n", cmd, ok);
}

// SEC-B3: parser estricto de entero/flotante con validación NaN
bool parseFloatStrict(const String& s, float& out) {
  if (s.length() == 0 || s.length() > 8) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (!(isdigit(c) || c == '.' || c == '-' || c == '+')) return false;
  }
  out = s.toFloat();
  return !isnan(out) && isfinite(out);
}

// SEC-B1/B2: verifica token antes de ejecutar cualquier comando
bool autorizado(const String& cmd) {
  int t = cmd.indexOf("\"TK\":\"");
  if (t < 0) return false;
  int ini = t + 6;
  int fin = cmd.indexOf('"', ini);
  if (fin < 0) return false;
  String tk = cmd.substring(ini, fin);
  // comparación en tiempo constante aproximado
  if (tk.length() != strlen(SHARED_TOKEN)) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < tk.length(); ++i)
    diff |= (uint8_t)(tk[i] ^ SHARED_TOKEN[i]);
  return diff == 0;
}

// SEC-B3/B4: parser de comando con validación estricta
void procesarComando(String cmd) {
  cmd.trim();
  if (cmd.length() == 0 || cmd.length() > RX_MAX) return;

  // SEC-B5: rate limiting
  unsigned long now = millis();
  if (now - ultimoComando < CMD_MIN_INTERVAL_MS) {
    Serial.println("[SEC] rate-limit: comando descartado");
    return;
  }
  ultimoComando = now;

  // SEC-B1/B2: autorización obligatoria
  if (!autorizado(cmd)) {
    Serial.println("[SEC] comando NO autorizado -> rechazado");
    enviarACK("UNAUTH", 0, false);
    return;
  }

  // --- SET_THRESHOLD ---
  if (cmd.startsWith("{\"CMD\":\"SET_THRESHOLD\"")) {
    int idx = cmd.indexOf("\"VAL\":");
    if (idx < 0) { enviarACK("SET_THRESHOLD", umbral, false); return; }
    int fin = cmd.indexOf(',', idx);
    if (fin < 0) fin = cmd.indexOf('}', idx);
    if (fin < 0) { enviarACK("SET_THRESHOLD", umbral, false); return; }
    float nuevo;
    if (!parseFloatStrict(cmd.substring(idx + 6, fin), nuevo) ||
        nuevo < UMBRAL_MIN || nuevo > UMBRAL_MAX) {
      enviarACK("SET_THRESHOLD", umbral, false);
      return;
    }
    umbral = nuevo;
    enviarACK("SET_THRESHOLD", umbral, true);
    return;
  }

  if (cmd.indexOf("\"CMD\":\"FAN_ON\"")  >= 0) {
    modoAuto = false; aplicarVentilador(true);
    enviarACK("FAN_ON", 1, true); return;
  }
  if (cmd.indexOf("\"CMD\":\"FAN_OFF\"") >= 0) {
    modoAuto = false; aplicarVentilador(false);
    enviarACK("FAN_OFF", 0, true); return;
  }
  if (cmd.indexOf("\"CMD\":\"MODE\"") >= 0) {
    bool ok = false;
    if (cmd.indexOf("\"VAL\":\"A\"") >= 0) { modoAuto = true;  ok = true; }
    if (cmd.indexOf("\"VAL\":\"M\"") >= 0) { modoAuto = false; ok = true; }
    enviarACK("MODE", modoAuto ? 1 : 0, ok); return;
  }
  if (cmd.indexOf("\"CMD\":\"STATUS\"") >= 0) { enviarTelemetria(); return; }

  enviarACK("UNKNOWN", 0, false);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  // SEC-B9: watchdog para recuperación de fallos
  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);   // fail-safe: relé apagado al boot

  dht.begin();
  // SEC-B1: emparejamiento con PIN (Secure Simple Pairing, "DisplayOnly")
  SerialBT.begin("NodoSensor_Temp");
  SerialBT.setPin("472913", 6);   // PIN numérico, no expuesto en logs
  Serial.println("[SEC] BT listo (PIN protegido)");
  ultimaConexion = millis();
}

// ---------- Loop ----------
void loop() {
  esp_task_wdt_reset();   // SEC-B9

  unsigned long ahora = millis();

  // SEC-B6: timeout de sesión BT
  if (!SerialBT.hasClient() && (ahora - ultimaConexion > BT_TIMEOUT_MS)) {
    Serial.println("[SEC] BT sin cliente: mantenido en estado seguro");
    if (ventilador) aplicarVentilador(false);
    bufferRX = "";
    ultimaConexion = ahora;
  }
  if (SerialBT.hasClient()) ultimaConexion = ahora;

  // Lectura periódica no bloqueante
  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = ahora;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      temperatura = t; humedad = h;
      if (modoAuto) {
        if (!ventilador && temperatura > umbral)       aplicarVentilador(true);
        if ( ventilador && temperatura < umbral - 1.0f) aplicarVentilador(false);
      }
    } else {
      Serial.println("[SEC] lectura DHT22 inválida");
    }
    enviarTelemetria();
  }

  // SEC-B9: fail-safe — si en modo AUTO no hay cliente, apagar ventilador
  if (modoAuto && !SerialBT.hasClient() &&
      (ahora - ultimaConexion > FAILSAFE_MS) && ventilador) {
    Serial.println("[SEC] fail-safe: ventilador apagado (sin enlace)");
    aplicarVentilador(false);
  }

  // Recepción de comandos (buffer acotado)
  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\n' || c == '\r') {
      if (bufferRX.length() > 0) {
        procesarComando(bufferRX);
        bufferRX = "";
      }
    } else if (bufferRX.length() < RX_MAX) {   // SEC-B10
      bufferRX += c;
    } else {
      Serial.println("[SEC] RX overflow: buffer descartado");
      bufferRX = "";
    }
  }
}
