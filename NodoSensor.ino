/*
 * NODO SENSOR - ESP32
 * - Lee DHT22 cada 2s sin bloquear (millis)
 * - Activa ventilador (relé) si T > umbral en modo AUTO
 * - Envía telemetría por Bluetooth Serial (BluetoothSerial)
 * - Recibe y valida comandos, confirma con ACK
 */

#include "BluetoothSerial.h"
#include "DHT.h"

// ---------- Pines ----------
#define DHTPIN      4
#define DHTTYPE     DHT22
#define RELAY_PIN   5

// ---------- Objetos ----------
BluetoothSerial SerialBT;
DHT dht(DHTPIN, DHTTYPE);

// ---------- Estado ----------
float temperatura = 0.0;
float humedad     = 0.0;
float umbral      = 30.0;      // °C
bool  ventilador  = false;
bool  modoAuto    = true;      // true = automático, false = manual

// ---------- Temporización (millis) ----------
const unsigned long INTERVALO_LECTURA = 2000;   // 2 s
unsigned long ultimaLectura = 0;

// ---------- Buffer para comandos ----------
String bufferRX = "";

// ---------- Utilidades ----------
void aplicarVentilador(bool estado) {
  ventilador = estado;
  digitalWrite(RELAY_PIN, estado ? HIGH : LOW);
  Serial.print("Ventilador: ");
  Serial.println(estado ? "ON" : "OFF");
}

// Genera y envía telemetría JSON
void enviarTelemetria() {
  String msg = "{";
  msg += "\"T\":"   + String(temperatura, 1) + ",";
  msg += "\"H\":"   + String(humedad, 1)     + ",";
  msg += "\"V\":"   + String(ventilador ? 1 : 0) + ",";
  msg += "\"U\":"   + String(umbral, 1)      + ",";
  msg += "\"M\":\"" + String(modoAuto ? "A" : "M") + "\"";
  msg += "}";
  SerialBT.println(msg);
  Serial.println("TX: " + msg);
}

// Respuesta de confirmación
void enviarACK(String cmd, float val, bool ok) {
  String msg = "{\"ACK\":\"" + cmd + "\",";
  msg += "\"VAL\":" + String(val, 1) + ",";
  msg += "\"OK\":" + String(ok ? 1 : 0) + "}";
  SerialBT.println(msg);
  Serial.println("TX ACK: " + msg);
}

// ---------- Procesamiento de comandos ----------
void procesarComando(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  // SET_THRESHOLD
  if (cmd.startsWith("{\"CMD\":\"SET_THRESHOLD\"")) {
    int idx = cmd.indexOf("\"VAL\":");
    if (idx < 0) { enviarACK("SET_THRESHOLD", umbral, false); return; }
    String valStr = cmd.substring(idx + 6, cmd.indexOf('}', idx));
    float nuevo = valStr.toFloat();
    // Validación de rango razonable
    if (nuevo >= 10.0 && nuevo <= 60.0) {
      umbral = nuevo;
      enviarACK("SET_THRESHOLD", umbral, true);
    } else {
      enviarACK("SET_THRESHOLD", umbral, false);
    }
    return;
  }

  // FAN_ON
  if (cmd.indexOf("\"CMD\":\"FAN_ON\"") >= 0) {
    modoAuto = false;
    aplicarVentilador(true);
    enviarACK("FAN_ON", 1, true);
    return;
  }

  // FAN_OFF
  if (cmd.indexOf("\"CMD\":\"FAN_OFF\"") >= 0) {
    modoAuto = false;
    aplicarVentilador(false);
    enviarACK("FAN_OFF", 0, true);
    return;
  }

  // MODE (A/M)
  if (cmd.indexOf("\"CMD\":\"MODE\"") >= 0) {
    bool ok = false;
    if (cmd.indexOf("\"VAL\":\"A\"") >= 0) { modoAuto = true;  ok = true; }
    if (cmd.indexOf("\"VAL\":\"M\"") >= 0) { modoAuto = false; ok = true; }
    enviarACK("MODE", modoAuto ? 1 : 0, ok);
    return;
  }

  // STATUS
  if (cmd.indexOf("\"CMD\":\"STATUS\"") >= 0) {
    enviarTelemetria();
    return;
  }

  // Comando desconocido
  enviarACK("UNKNOWN", 0, false);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  Serial.println("Nodo Sensor iniciando...");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);   // Ventilador apagado al inicio

  dht.begin();
  SerialBT.begin("NodoSensor_Temp");  // Nombre Bluetooth visible
  Serial.println("Bluetooth listo: NodoSensor_Temp");
}

// ---------- Loop ----------
void loop() {
  // 1) Lectura periódica NO bloqueante
  unsigned long ahora = millis();
  if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
    ultimaLectura = ahora;

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      temperatura = t;
      humedad     = h;

      // Lógica AUTO: si supera umbral → activar; con histéresis 1°C para evitar parpadeo
      if (modoAuto) {
        if (!ventilador && temperatura > umbral)       aplicarVentilador(true);
        if ( ventilador && temperatura < umbral - 1.0) aplicarVentilador(false);
      }
    } else {
      Serial.println("Error leyendo DHT22");
    }

    enviarTelemetria();
  }

  // 2) Recepción de comandos por Bluetooth
  while (SerialBT.available()) {
    char c = SerialBT.read();
    if (c == '\n' || c == '\r') {
      if (bufferRX.length() > 0) {
        procesarComando(bufferRX);
        bufferRX = "";
      }
    } else {
      if (bufferRX.length() < 200) bufferRX += c;  // evitar overflow
    }
  }
}
