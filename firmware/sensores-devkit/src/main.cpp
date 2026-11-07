// =====================================================================
//  PLACA DE SENSORES - ESP32 DevKit clasico
//  WiFi + sensor de lluvia + sensor de nivel + MQTT + RELE de la V9
//  v4.1.3
// =====================================================================
//
//  V9 "Entrada calle" = entrada de agua al deposito. NORMALMENTE ABIERTA.
//   - Siempre abierta. Solo se cierra a mano si hay una averia.
//   - El estado "cerrada por averia" se GUARDA: si la placa se reinicia
//     estando cerrada, vuelve a cerrarla. (Sin luz, al ser NA, se abre.)
//   - Sin cierres automaticos.
//
//  NOVEDADES v4
//   - Actualizacion por WiFi (OTA) con contrasena: "riego-sensores".
//   - Vuelta atras automatica si el firmware nuevo no conecta al WiFi.
//   - Vigilante: si el programa se cuelga 60 s, la placa se reinicia.
//   - Busca el broker solo en la red (mDNS "riego"); si no, IP fija.
//   - Informe cada 60 s en riego/sensores/info.
//   - Usuario/clave MQTT opcionales (MQTT_USER / MQTT_PASS en secretos.h).
//
//  MQTT
//   escucha : riego/valvula/9/set      "ON"  = ABIERTA   "OFF" = CERRADA (averia)
//   publica : riego/valvula/9/estado   "ON" / "OFF" (retenido)
//             riego/clima/lluvia       "ON" / "OFF" (retenido)
//             riego/deposito/nivel     0-100 (retenido)
//             riego/sensores/info      JSON cada 60 s (retenido)
//   LWT     : riego/valvula/9/estado = "ON" (sin placa, la NA queda abierta)
//
//  CABLEADO DEL RELE: IN -> GPIO 23, VCC -> 5V, GND -> GND
//   Bobina por COM + NO: rele en reposo = valvula abierta.
// =====================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_attr.h"

#include "secretos.h"
#ifndef OTA_PASS
#error "Falta OTA_PASS en include/secretos.h (mira include/secretos.example.h)"
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#define MQTT_PASS ""
#endif

#define FW_VERSION "sensores-devkit 4.1.3"
#define HOSTNAME   "riego-sensores"

const char* MQTT_FALLBACK_IP = "192.168.1.100";

// ---- Redes WiFi conocidas: la de secretos.h (ssid/password) y, si existe, una segunda
//      (WIFI2_SSID / WIFI2_PASS en secretos.h). Se conecta a la que tenga mejor senal.
#ifndef MQTT_FALLBACK_IP_2
#define MQTT_FALLBACK_IP_2 ""            // IP fija del broker en la red 2 (vacio = la misma)
#endif
// Red 3 (opcional): mismo nombre que otra pero otra contrasena (p. ej. una red antigua
// mientras se cambia el router). Si una contrasena falla, al minuto prueba la otra.
#if defined(WIFI2_SSID) && defined(WIFI3_SSID)
const char* REDES_SSID[] = {ssid, WIFI2_SSID, WIFI3_SSID};
const char* REDES_PASS[] = {password, WIFI2_PASS, WIFI3_PASS};
const int NUM_REDES = 3;
#elif defined(WIFI2_SSID)
const char* REDES_SSID[] = {ssid, WIFI2_SSID};
const char* REDES_PASS[] = {password, WIFI2_PASS};
const int NUM_REDES = 2;
#else
const char* REDES_SSID[] = {ssid};
const char* REDES_PASS[] = {password};
const int NUM_REDES = 1;
#endif
int redActual = 0;          // red a la que se ha intentado conectar
int intentoWifi = 0;        // cuenta de intentos: va rotando entre las redes candidatas

void empezarWiFi() {
  // Candidatas = redes conocidas que se ven, ordenadas por senal. Cada intento fallido
  // (60 s sin conectar) pasa a la siguiente: asi, si una contrasena no vale, prueba la otra.
  int cand[NUM_REDES]; int rssi[NUM_REDES]; int nc = 0;
  if (NUM_REDES > 1) {
    int n = WiFi.scanNetworks();
    for (int r = 0; r < NUM_REDES; r++) {
      int mejor = -1000;
      for (int i = 0; i < n; i++)
        if (WiFi.SSID(i) == REDES_SSID[r] && WiFi.RSSI(i) > mejor) mejor = WiFi.RSSI(i);
      if (mejor > -1000) {
        int j = nc++;
        while (j > 0 && rssi[j - 1] < mejor) { cand[j] = cand[j - 1]; rssi[j] = rssi[j - 1]; j--; }
        cand[j] = r; rssi[j] = mejor;
      }
    }
    WiFi.scanDelete();
  }
  int mejor = (nc > 0) ? cand[intentoWifi % nc] : (intentoWifi % NUM_REDES);
  intentoWifi++;
  redActual = mejor;
  Serial.printf("WiFi: conectando a %s\n", REDES_SSID[mejor]);
  WiFi.begin(REDES_SSID[mejor], REDES_PASS[mejor]);
}

const char* ipBrokerFija() {
  return (redActual >= 1 && strlen(MQTT_FALLBACK_IP_2) > 0) ? MQTT_FALLBACK_IP_2 : MQTT_FALLBACK_IP;
}
const uint16_t MQTT_FALLBACK_PORT = 1883;

WiFiClient espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

// --- Sensor de lluvia (digital) ---
#define PIN_LLUVIA 17

// --- JSN-SR04T / AJ-SR04M ---
#define PIN_TRIG 19
#define PIN_ECHO 18   // con divisor de tension 1k/2k

// --- RELE / VALVULA ---
#define PIN_RELE         23
#define RELE_ACTIVO_BAJO 1   // 1 = el modulo activa con LOW
#define VALVULA_NA       1   // 1 = normalmente ABIERTA
#define VALVULA_ID       9

char TOPIC_SET[40];
char TOPIC_ESTADO[44];

bool abierta = true;               // estado logico: true = pasa el agua
unsigned long tSuscrito = 0;
unsigned long tEstadoPeriodico = 0;

// CALIBRACION DEL DEPOSITO (en cm)
const float CM_DEPOSITO_VACIO = 100.0;
const float CM_DEPOSITO_LLENO = 25.0;
const float DIST_MIN_CM = 20.0;
const float DIST_MAX_CM = 450.0;

unsigned long tLluvia = 0;
unsigned long tNivel = 0;
unsigned long tInfo = 0;
unsigned long tWifiInicio = 0;
unsigned long tMqttRetry = 0;
bool wifiAvisado = false;
bool otaIniciada = false;
int fallosMqtt = 0;
String mdnsDiag = "";         // lo que ha visto la busqueda mDNS (se manda como evento)
int rondaBroker = 0;        // que broker toca probar (se pone a 0 al conectar)
bool brokerResuelto = false;
String brokerOrigen = "";
int ultimoNivel = -1;
bool ultimaLluvia = false;

// --- Vigilante y vuelta atras ---
hw_timer_t* wdt = NULL;
RTC_NOINIT_ATTR uint32_t marcaBloqueo;
String motivoArranque = "";
bool pendienteVerificar = false;

extern "C" bool verifyRollbackLater() { return true; }

void IRAM_ATTR reiniciarPorBloqueo() {
  marcaBloqueo = 0xB10C;
  esp_restart();
}

inline void alimentarVigilante() { if (wdt) timerWrite(wdt, 0); }

// ---------- Valvula ----------
void publicarEstado() {
  if (mqtt.connected()) mqtt.publish(TOPIC_ESTADO, abierta ? "ON" : "OFF", true);
}

void escribirRele(bool abrir) {
  bool bobinaAlimentada = VALVULA_NA ? !abrir : abrir;
  bool nivelPin = RELE_ACTIVO_BAJO ? !bobinaAlimentada : bobinaAlimentada;
  digitalWrite(PIN_RELE, nivelPin ? HIGH : LOW);
}

void aplicarValvula(bool abrir, bool guardar) {
  escribirRele(abrir);
  abierta = abrir;
  if (guardar) prefs.putBool("v9cerr", !abrir);
  Serial.printf("[Valvula %d] %s\n", VALVULA_ID, abrir ? "ABIERTA (pasa el agua)" : "CERRADA (averia)");
  publicarEstado();
}

void alRecibir(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  msg.toUpperCase();
  Serial.printf("[MQTT] %s = %s\n", topic, msg.c_str());

  // Orden RETENIDA que llega nada mas suscribirse: se ignora
  if (millis() - tSuscrito < 1500) {
    Serial.println("[MQTT] Orden recibida justo al conectar: la ignoro (posible retenida)");
    return;
  }
  if (msg == "ON" || msg == "1" || msg == "TRUE") aplicarValvula(true, true);
  else if (msg == "OFF" || msg == "0" || msg == "FALSE") aplicarValvula(false, true);
}

// ---------- Conexiones ----------
void iniciarOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    if (mqtt.connected()) mqtt.publish("riego/sensores/evento", "Actualizando firmware por WiFi...");
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { alimentarVigilante(); });
  ArduinoOTA.begin();
  otaIniciada = true;
  Serial.println("[OTA] Lista como " HOSTNAME);
}

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiAvisado) {
      Serial.printf("WiFi OK: red %s, IP %s, senal %d dBm\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
      wifiAvisado = true;
      if (!otaIniciada) iniciarOTA();
    }
    tWifiInicio = millis();
    return;
  }
  wifiAvisado = false;
  if (millis() - tWifiInicio > 60000) {
    Serial.println("WiFi: 60 s sin conexion, reinicio la conexion");
    WiFi.disconnect();
    empezarWiFi();
    tWifiInicio = millis();
  }
}

void resolverBroker() {
  IPAddress ip;
  ip.fromString(ipBrokerFija());
  uint16_t port = MQTT_FALLBACK_PORT;
  String origen = "IP fija";
  if (otaIniciada) {   // mDNS ya arrancado por ArduinoOTA
    // Busco brokers que anuncien "_mqtt._tcp" con la marca riego (TXT riego=1) o con "riego" en el nombre.
    // Si hay varios, van por prioridad (TXT prio, mayor primero). Si el elegido falla 3 veces,
    // en la siguiente vuelta pruebo el siguiente, y al final la IP fija.
    int n = MDNS.queryService("mqtt", "tcp");
    int cand[6]; int prio[6]; int nc = 0;
    IPAddress ipc[6];
    mdnsDiag = "mDNS: " + String(n) + " broker(s)";
    for (int i = 0; i < n && nc < 6; i++) {
      bool marca = MDNS.hasTxt(i, "riego") || MDNS.hostname(i).indexOf("riego") >= 0;
      IPAddress dir = MDNS.IP(i);
      if (marca && dir == IPAddress(0, 0, 0, 0)) dir = MDNS.queryHost(MDNS.hostname(i), 2000);  // sin IP en la respuesta: la pido
      mdnsDiag += " | " + MDNS.hostname(i) + " " + dir.toString() + (marca ? " riego" : " (no es del riego)");
      if (!marca || dir == IPAddress(0, 0, 0, 0)) continue;
      int p = MDNS.hasTxt(i, "prio") ? MDNS.txt(i, "prio").toInt() : 0;
      int j = nc++;
      while (j > 0 && prio[j - 1] < p) { cand[j] = cand[j - 1]; prio[j] = prio[j - 1]; ipc[j] = ipc[j - 1]; j--; }
      cand[j] = i; prio[j] = p; ipc[j] = dir;
    }
    Serial.println("[MQTT] " + mdnsDiag);
    int elegido = rondaBroker % (nc + 1);    // nc = usar la IP fija
    rondaBroker++;
    if (elegido < nc) {
      int i = cand[elegido];
      ip = ipc[elegido];
      port = MDNS.port(i);
      origen = "mDNS " + MDNS.hostname(i) + " prio " + prio[elegido];
    }
  }
  brokerOrigen = origen + " (" + ip.toString() + ":" + port + ")";
  mqtt.setServer(ip, port);
  Serial.println("[MQTT] Broker: " + brokerOrigen);
}

void conectarMQTT() {
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (millis() - tMqttRetry < 5000) return;
  tMqttRetry = millis();

  if (!brokerResuelto || fallosMqtt >= 3) {
    resolverBroker();
    brokerResuelto = true;
    fallosMqtt = 0;
  }

  Serial.print("Conectando a MQTT...");
  bool conUsuario = strlen(MQTT_USER) > 0;
  // LWT: si la placa desaparece, una valvula NA queda abierta (ON)
  bool ok = mqtt.connect("PuenteSensoresDevKit",
                         conUsuario ? MQTT_USER : NULL, conUsuario ? MQTT_PASS : NULL,
                         TOPIC_ESTADO, 0, true, VALVULA_NA ? "ON" : "OFF");
  alimentarVigilante();
  if (ok) {
    Serial.println(" OK");
    fallosMqtt = 0;
    rondaBroker = 0;
    if (mdnsDiag.length()) { mqtt.publish("riego/sensores/evento", ("Broker: " + brokerOrigen + " | " + mdnsDiag).substring(0, 200).c_str()); mdnsDiag = ""; }
    mqtt.subscribe(TOPIC_SET);
    tSuscrito = millis();
    publicarEstado();
  } else {
    fallosMqtt++;
    Serial.print(" FALLO, rc=");
    Serial.println(mqtt.state());
  }
}

void publicarInfo() {
  if (!mqtt.connected()) return;
  char b[420];
  snprintf(b, sizeof(b),
    "{\"version\":\"%s\",\"uptime_s\":%lu,\"ip\":\"%s\",\"rssi\":%d,\"broker\":\"%s\","
    "\"arranque\":\"%s\",\"valvula9_abierta\":%s,\"lluvia\":%s,\"deposito\":%d}",
    FW_VERSION, millis() / 1000, WiFi.localIP().toString().c_str(), WiFi.RSSI(),
    brokerOrigen.c_str(), motivoArranque.c_str(),
    abierta ? "true" : "false", ultimaLluvia ? "true" : "false", ultimoNivel);
  mqtt.publish("riego/sensores/info", b, true);
}

// ---------- Nivel ----------
float medirUnaVez() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long duracion = pulseIn(PIN_ECHO, HIGH, 30000);
  if (duracion == 0) return -1;
  return (duracion * 0.0343f) / 2.0f;
}

float medirDistanciaCM() {
  float lecturas[5];
  int n = 0;
  for (int i = 0; i < 5; i++) {
    float d = medirUnaVez();
    if (d >= DIST_MIN_CM && d <= DIST_MAX_CM) lecturas[n++] = d;
    delay(60);
    if (mqtt.connected()) mqtt.loop();
  }
  if (n == 0) return -1;
  for (int i = 1; i < n; i++) {
    float v = lecturas[i];
    int j = i - 1;
    while (j >= 0 && lecturas[j] > v) { lecturas[j + 1] = lecturas[j]; j--; }
    lecturas[j + 1] = v;
  }
  return lecturas[n / 2];
}

String calcularMotivoArranque() {
  esp_reset_reason_t r = esp_reset_reason();
  String m;
  if (r == ESP_RST_SW && marcaBloqueo == 0xB10C) m = "vigilante (programa colgado)";
  else switch (r) {
    case ESP_RST_POWERON:  m = "encendido"; break;
    case ESP_RST_SW:       m = "reinicio por software"; break;
    case ESP_RST_PANIC:    m = "fallo del programa"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      m = "watchdog del sistema"; break;
    case ESP_RST_BROWNOUT: m = "bajada de tension"; break;
    default:               m = "otro (" + String((int)r) + ")"; break;
  }
  marcaBloqueo = 0;
  return m;
}

void setup() {
  // Primero el rele, con el estado guardado (abierta salvo averia)
  prefs.begin("riego", false);
  bool cerradaPorAveria = prefs.getBool("v9cerr", false);
  escribirRele(!cerradaPorAveria);
  pinMode(PIN_RELE, OUTPUT);
  escribirRele(!cerradaPorAveria);
  abierta = !cerradaPorAveria;

  Serial.begin(115200);
  motivoArranque = calcularMotivoArranque();

  wdt = timerBegin(0, 80, true);
  timerAttachInterrupt(wdt, &reiniciarPorBloqueo, true);
  timerAlarmWrite(wdt, 60ULL * 1000000ULL, false);
  timerAlarmEnable(wdt);

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t estadoOta;
  if (esp_ota_get_state_partition(running, &estadoOta) == ESP_OK && estadoOta == ESP_OTA_IMG_PENDING_VERIFY) {
    pendienteVerificar = true;
  }

  snprintf(TOPIC_SET, sizeof(TOPIC_SET), "riego/valvula/%d/set", VALVULA_ID);
  snprintf(TOPIC_ESTADO, sizeof(TOPIC_ESTADO), "riego/valvula/%d/estado", VALVULA_ID);

  pinMode(PIN_LLUVIA, INPUT_PULLUP);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  Serial.printf("\n%s | arranque: %s | valvula 9 %s\n", FW_VERSION, motivoArranque.c_str(),
                abierta ? "ABIERTA" : "CERRADA (averia guardada)");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  empezarWiFi();
  tWifiInicio = millis();
  mqtt.setCallback(alRecibir);
  mqtt.setBufferSize(512);   // el informe con el nombre del broker no cabe en los 256 de serie
  mqtt.setKeepAlive(60);     // 60 s: aguanta cortes cortos del WiFi sin desconectarse
  mqtt.setSocketTimeout(10);
}

void loop() {
  alimentarVigilante();
  conectarWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    conectarMQTT();
    if (otaIniciada) ArduinoOTA.handle();
  }

  if (pendienteVerificar) {
    if (WiFi.status() == WL_CONNECTED && millis() > 60000) {
      esp_ota_mark_app_valid_cancel_rollback();
      pendienteVerificar = false;
      if (mqtt.connected()) mqtt.publish("riego/sensores/evento", "Firmware nuevo confirmado: " FW_VERSION);
    } else if (millis() > 180000) {
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }

  if (millis() - tEstadoPeriodico > 60000) {
    publicarEstado();
    tEstadoPeriodico = millis();
  }

  // Lluvia y nivel: se leen igual de a menudo, pero solo se publican si cambian o cada 60 s
  static unsigned long tLluviaPub = 0, tNivelPub = 0;
  static int lluviaPublicada = -1, nivelPublicado = -1;
  if (millis() - tLluvia > 5000) {
    if (mqtt.connected()) {
      ultimaLluvia = (digitalRead(PIN_LLUVIA) == LOW);
      if (ultimaLluvia != lluviaPublicada || tLluviaPub == 0 || millis() - tLluviaPub > 60000) {
        mqtt.publish("riego/clima/lluvia", ultimaLluvia ? "ON" : "OFF", true);
        Serial.printf("[Clima] Lluvia: %s\n", ultimaLluvia ? "ON" : "OFF");
        lluviaPublicada = ultimaLluvia;
        tLluviaPub = millis();
      }
    }
    tLluvia = millis();
  }

  if (millis() - tNivel > 10000) {
    if (mqtt.connected()) {
      float distancia = medirDistanciaCM();
      if (distancia > 0) {
        float pct = (CM_DEPOSITO_VACIO - distancia) / (CM_DEPOSITO_VACIO - CM_DEPOSITO_LLENO) * 100.0f;
        ultimoNivel = (int)constrain(pct + 0.5f, 0.0f, 100.0f);
        if (ultimoNivel != nivelPublicado || tNivelPub == 0 || millis() - tNivelPub > 60000) {
          char payload[10];
          snprintf(payload, sizeof(payload), "%d", ultimoNivel);
          mqtt.publish("riego/deposito/nivel", payload, true);
          Serial.printf("[Nivel] %.1f cm -> %d%%\n", distancia, ultimoNivel);
          nivelPublicado = ultimoNivel;
          tNivelPub = millis();
        }
      } else {
        Serial.println("[Nivel] Sin lectura valida del sensor");
      }
    }
    tNivel = millis();
  }

  if (millis() - tInfo > 60000) {
    publicarInfo();
    tInfo = millis();
  }

  delay(10);
}
