// =====================================================================
//  CENTRAL DE RIEGO - ESP32-S3 (pantalla ILI9341 + LVGL + 8 reles)
//  v2.3.2
// =====================================================================
//
//  VALVULAS
//   V1  (rele 1) Entrada a la bomba. NORMALMENTE ABIERTA (NA).
//       Siempre abierta. Solo se cierra a mano si hay una averia.
//       El estado "cerrada por averia" se guarda y se respeta al reiniciar.
//       OJO: al ser NA, si se va la luz se abre sola (sin corriente).
//   V2-V8 (reles 2-8) Zonas de riego (normalmente cerradas).
//       Manual (pantalla, web, Jarvis): maximo 30 min y se cierra sola.
//       Programas: maximo 120 min, en turnos (una zona detras de otra).
//   V9  Entrada calle: va en la placa DevKit. Aqui solo se manda la orden.
//
//  NOVEDADES v2
//   - Programas guardados en memoria permanente (no se pierden sin luz).
//   - Programas con dias de la semana y riego por turnos.
//   - Programas editables desde el servidor (riego/s3/programas/set).
//   - Interruptor "Inteligente" que se guarda (riego/s3/inteligente).
//   - No riega en automatico si: llueve, V1 cerrada o deposito < 10 %.
//   - Sensores activables/desactivables con un interruptor en la pestana
//     Sensores (y por riego/sensor/N/activo). Desactivado = sin avisos.
//   - Actualizacion por WiFi (OTA) con contrasena. No se actualiza
//     mientras hay una zona regando.
//   - Vuelta atras automatica si el firmware nuevo no conecta al WiFi.
//   - Vigilante: si el programa se cuelga 60 s, la placa se reinicia.
//   - Busca el broker solo en la red (mDNS "riego"); si no, IP fija.
//   - Informe cada 60 s en riego/s3/info y aviso de caida en riego/s3/online.
//
//  TOPICS MQTT
//   escucha  riego/valvula/{1-8}/set     V1: ON=abrir OFF=cerrar
//                                        V2-8: ON=30 min, numero=minutos, OFF
//            riego/valvula/9/estado      estado de la entrada calle (DevKit)
//            riego/sensores/nodoN        humedad de suelo (0-100)
//            riego/sensor/N/activo       "1" / "0"  (retenido)
//            riego/clima/lluvia          ON / OFF
//            riego/deposito/nivel        0-100
//            riego/s3/programas/set      JSON (ver publicarProgramas)
//            riego/s3/inteligente/set    ON / OFF
//   publica  riego/valvula/{1-8}/estado  (retenido)
//            riego/clima/temperatura, riego/clima/humedad
//            riego/s3/programas          JSON (retenido)
//            riego/s3/inteligente        ON / OFF (retenido)
//            riego/s3/info               JSON cada 60 s (retenido)
//            riego/s3/online             "1" / "0" (ultima voluntad)
//            riego/s3/evento             texto: que ha hecho y por que
// =====================================================================

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include <DHT.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "time.h"

// --- Claves (no van a Git) ---
#include "secretos.h"
#ifndef OTA_PASS
#error "Falta OTA_PASS en include/secretos.h (mira include/secretos.example.h)"
#endif

#define FW_VERSION  "s3-central 2.3.2"
#define HOSTNAME    "riego-s3"

// --- Red / MQTT ---
const char* MQTT_FALLBACK_IP = "192.168.1.100";   // si no se encuentra por mDNS

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
const char* ntpServer = "pool.ntp.org";
const char* tz_info   = "WET0WEST,M3.5.0/1,M10.5.0";

WiFiClient espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

// --- PINES HARDWARE ---
#define SCLK_PIN 13
#define MISO_PIN 12
#define MOSI_PIN 11
#define DC_PIN    9
#define CS_PIN   10
#define RST_PIN  14
#define TOUCH_CS 16
#define PIN_DHT   4

Adafruit_ILI9341 tft = Adafruit_ILI9341(&SPI, DC_PIN, CS_PIN, RST_PIN);
XPT2046_Touchscreen ts(TOUCH_CS);
DHT dht(PIN_DHT, DHT11);

// --- RELES Y VALVULAS ---
const int  NUM_RELES = 8;
const int  PINES_RELES[NUM_RELES] = {5, 6, 7, 8, 17, 18, 21, 38};
const bool RELE_ACTIVO_ALTO = true;     // el modulo activa el rele con HIGH
// true = valvula normalmente ABIERTA (se cierra al alimentar la bobina)
const bool VALVULA_NA[NUM_RELES] = {true, false, false, false, false, false, false, false};

const char* NOMBRES_VALVULAS[9] = {
  "1. Entrada bomba", "2. Arboles", "3. Huerto", "4. Parterre",
  "5. P. Superior",   "6. Aux 6",   "7. Aux 7",  "8. Aux 8",
  "9. Entrada calle"
};
const int IDX_ENTRADA = 0;          // V1
const int VALVULA_REMOTA_IDX = 8;   // V9 (DevKit)

// --- Limites de seguridad ---
const uint32_t MAX_MANUAL_MIN  = 30;    // riego manual: se cierra solo a los 30 min
const uint32_t MAX_PROG_MIN    = 120;   // un programa nunca riega mas de 2 h seguidas
const int      DEPOSITO_MIN_PCT = 10;   // por debajo no se riega en automatico
const unsigned long NIVEL_VALIDO_MS = 30UL * 60000UL;  // dato de deposito "reciente"
const unsigned long TIMEOUT_NODO = 7200000UL;          // 2 h sin dato = sin senal
const uint32_t WDT_SEGUNDOS = 60;

// --- Estado de riego ---
enum Origen : uint8_t { ORIG_MANUAL = 0, ORIG_AUTO = 1, ORIG_REMOTO = 2 };

struct Riego {
  bool activo = false;
  unsigned long inicio = 0;
  unsigned long durMs = 0;
  uint8_t origen = ORIG_MANUAL;
};
Riego riegos[NUM_RELES];

bool entradaCerrada = false;     // V1 cerrada por averia
bool modoInteligente = false;
// Ajuste del riego inteligente (lo manda el servidor cada dia, en % por zona).
// Solo se aplica con el interruptor "Riego inteligente" activado y mientras no caduque.
uint8_t ajusteZona[8] = {100, 100, 100, 100, 100, 100, 100, 100};
time_t ajusteHasta = 0;
bool ajusteVigente() {
  time_t ahora = time(nullptr);
  return ajusteHasta > 0 && ahora > 1700000000 && ahora < ajusteHasta;
}

// Programas: [valvula 0-7][3 programas]. Solo se usan las zonas 1-7 (V2-V8).
struct ProgramaRiego {
  uint8_t hora;
  uint8_t minuto;
  uint8_t duracion;   // minutos, 0 = desactivado
  uint8_t dias;       // bit0 = lunes ... bit6 = domingo
};
ProgramaRiego horarios[NUM_RELES][3];

// Cola de riego por turnos
struct Pendiente { int8_t zona; uint8_t minutos; };
Pendiente cola[NUM_RELES * 3];
int nCola = 0;

// --- Sensores ---
int humedadNodos[8] = {0};
unsigned long ultimaVezVisto[8] = {0};
uint8_t sensorMask = 0xFF;      // bit N = sensor N+1 activo

float tempAire = NAN, humAire = NAN;
int nivelDeposito = -1;          // -1 = sin dato
unsigned long tNivel = 0;
bool lloviendo = false;

// --- Conexion ---
IPAddress brokerIP;
uint16_t brokerPort = MQTT_FALLBACK_PORT;
String brokerOrigen = "";
bool brokerResuelto = false;
int fallosMqtt = 0;
String mdnsDiag = "";         // lo que ha visto la busqueda mDNS (se manda como evento)
int rondaBroker = 0;        // que broker toca probar (se pone a 0 al conectar)
unsigned long tMqttRetry = 0;
unsigned long tSuscrito = 0;
unsigned long tWifiInicio = 0;
bool otaIniciada = false;

// --- Vigilante y vuelta atras ---
hw_timer_t* wdt = NULL;
RTC_NOINIT_ATTR uint32_t marcaBloqueo;
String motivoArranque = "";
bool pendienteVerificar = false;

// Le dice al nucleo que la verificacion del firmware nuevo la hacemos nosotros
extern "C" bool verifyRollbackLater() { return true; }

void IRAM_ATTR reiniciarPorBloqueo() {
  marcaBloqueo = 0xB10C;
  esp_restart();
}

inline void alimentarVigilante() { if (wdt) timerWrite(wdt, 0); }

// --- INTERFAZ (LVGL) ---
static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 240;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10];

lv_obj_t * label_meteo;
lv_obj_t * label_deposito_pct;
lv_obj_t * bar_deposito;
lv_obj_t * label_nodos[8];
lv_obj_t * switch_sensores[8];
lv_obj_t * label_version;
lv_obj_t * switch_valvulas[9];
lv_obj_t * switch_inteligente;
lv_obj_t * label_reloj;
lv_obj_t * dd_zonas;
lv_obj_t * dd_progs;
lv_obj_t * slider_hora;
lv_obj_t * slider_min;
lv_obj_t * slider_duracion;
lv_obj_t * label_hora_sel;
lv_obj_t * label_min_sel;
lv_obj_t * label_dur_sel;

// =====================================================================
//  Utilidades
// =====================================================================
void evento(const String& txt) {
  Serial.println("[Evento] " + txt);
  if (mqtt.connected()) mqtt.publish("riego/s3/evento", txt.c_str());
}

void publicarEstadoValvula(int idx, const char* estado) {
  if (!mqtt.connected()) return;
  char topic[40];
  snprintf(topic, sizeof(topic), "riego/valvula/%d/estado", idx + 1);
  mqtt.publish(topic, estado, true);
}

void actualizarSwitch(int idx, bool on) {
  if (idx < 0 || idx > 8 || switch_valvulas[idx] == NULL) return;
  if (on) lv_obj_add_state(switch_valvulas[idx], LV_STATE_CHECKED);
  else    lv_obj_clear_state(switch_valvulas[idx], LV_STATE_CHECKED);
}

void aplicarSalida(int idx, bool abierta) {
  bool bobina = VALVULA_NA[idx] ? !abierta : abierta;
  digitalWrite(PINES_RELES[idx], (bobina == RELE_ACTIVO_ALTO) ? HIGH : LOW);
}

bool hayRiegoActivo() {
  for (int i = 1; i < NUM_RELES; i++) if (riegos[i].activo) return true;
  return false;
}

int zonasRegando() {
  int n = 0;
  for (int i = 1; i < NUM_RELES; i++) if (riegos[i].activo) n++;
  return n;
}

bool hayRiegoAuto() {
  for (int i = 1; i < NUM_RELES; i++)
    if (riegos[i].activo && riegos[i].origen == ORIG_AUTO) return true;
  return false;
}

bool sensorActivo(int i) { return (sensorMask >> i) & 1; }

// =====================================================================
//  Memoria permanente
// =====================================================================
void programasPorDefecto() {
  for (int i = 0; i < NUM_RELES; i++)
    for (int p = 0; p < 3; p++)
      horarios[i][p] = {6, 0, 0, 0x7F};
}

void cargarMemoria() {
  prefs.begin("riego", false);
  programasPorDefecto();
  if (prefs.getBytesLength("prog") == sizeof(horarios)) {
    prefs.getBytes("prog", horarios, sizeof(horarios));
  }
  entradaCerrada  = prefs.getBool("v1cerr", false);
  modoInteligente = prefs.getBool("intel", false);
  sensorMask      = prefs.getUChar("sens", 0xFF);
}

void guardarProgramas() { prefs.putBytes("prog", horarios, sizeof(horarios)); }

// =====================================================================
//  Valvulas
// =====================================================================
void vaciarCola() { nCola = 0; }

void pararRiego(int idx, const char* motivo) {
  if (idx < 1 || idx >= NUM_RELES) return;
  bool estaba = riegos[idx].activo;
  uint8_t origen = riegos[idx].origen;
  riegos[idx].activo = false;
  aplicarSalida(idx, false);
  actualizarSwitch(idx, false);
  publicarEstadoValvula(idx, (estaba && origen == ORIG_AUTO) ? "OFF (AUTO)" : "OFF");
  if (estaba) evento(String("Cierro ") + NOMBRES_VALVULAS[idx] + ": " + motivo);
}

void pararTodasLasZonas(const char* motivo) {
  vaciarCola();
  for (int i = 1; i < NUM_RELES; i++) if (riegos[i].activo) pararRiego(i, motivo);
}

void iniciarRiego(int idx, uint32_t minutos, uint8_t origen) {
  if (idx < 1 || idx >= NUM_RELES || minutos == 0) return;
  uint32_t tope = (origen == ORIG_MANUAL) ? MAX_MANUAL_MIN : MAX_PROG_MIN;
  if (minutos > tope) minutos = tope;
  riegos[idx].activo = true;
  riegos[idx].inicio = millis();
  riegos[idx].durMs  = minutos * 60000UL;
  riegos[idx].origen = origen;
  aplicarSalida(idx, true);
  actualizarSwitch(idx, true);
  publicarEstadoValvula(idx, origen == ORIG_AUTO ? "ON (AUTO)" : "ON");
  const char* txtOrigen = origen == ORIG_AUTO ? "programa" : (origen == ORIG_MANUAL ? "manual" : "remoto");
  evento(String("Abro ") + NOMBRES_VALVULAS[idx] + " " + minutos + " min (" + txtOrigen + ")");
}

void setEntrada(bool abierta, const char* origen) {
  entradaCerrada = !abierta;
  aplicarSalida(IDX_ENTRADA, abierta);
  prefs.putBool("v1cerr", entradaCerrada);
  actualizarSwitch(IDX_ENTRADA, abierta);
  publicarEstadoValvula(IDX_ENTRADA, abierta ? "ON" : "OFF");
  if (!abierta) {
    pararTodasLasZonas("entrada de la bomba cerrada");
    evento(String("Entrada bomba CERRADA (averia) desde ") + origen);
  } else {
    evento(String("Entrada bomba ABIERTA desde ") + origen);
  }
}

// Motivo por el que NO se puede regar en automatico ("" = se puede)
String bloqueoAutomatico() {
  if (entradaCerrada) return "entrada de la bomba cerrada";
  if (lloviendo) return "esta lloviendo";
  if (nivelDeposito >= 0 && millis() - tNivel < NIVEL_VALIDO_MS && nivelDeposito < DEPOSITO_MIN_PCT)
    return String("deposito bajo (") + nivelDeposito + " %)";
  return "";
}

void encolarRiego(int idx, uint8_t minutos) {
  if (!hayRiegoAuto()) { iniciarRiego(idx, minutos, ORIG_AUTO); return; }
  for (int i = 0; i < nCola; i++) if (cola[i].zona == idx) return;  // ya esta en cola
  if (nCola < (int)(sizeof(cola) / sizeof(cola[0]))) {
    cola[nCola++] = {(int8_t)idx, minutos};
    evento(String(NOMBRES_VALVULAS[idx]) + " en espera de turno");
  }
}

void revisarTemporizadores() {
  unsigned long ahora = millis();
  for (int i = 1; i < NUM_RELES; i++) {
    if (riegos[i].activo && ahora - riegos[i].inicio >= riegos[i].durMs) {
      pararRiego(i, riegos[i].origen == ORIG_MANUAL ? "fin del tiempo maximo manual" : "fin de la duracion");
    }
  }
  if (nCola > 0 && !hayRiegoAuto()) {
    String motivo = bloqueoAutomatico();
    if (motivo.length()) { evento("Cancelo turnos pendientes: " + motivo); vaciarCola(); return; }
    Pendiente p = cola[0];
    for (int i = 1; i < nCola; i++) cola[i - 1] = cola[i];
    nCola--;
    iniciarRiego(p.zona, p.minutos, ORIG_AUTO);
  }
}

void revisarProgramas(const struct tm& t) {
  uint8_t bitDia = (t.tm_wday == 0) ? 6 : (t.tm_wday - 1);   // lunes = bit0
  for (int i = 1; i < NUM_RELES; i++) {
    for (int p = 0; p < 3; p++) {
      const ProgramaRiego& pr = horarios[i][p];
      if (pr.duracion == 0 || !((pr.dias >> bitDia) & 1)) continue;
      if (pr.hora != t.tm_hour || pr.minuto != t.tm_min) continue;
      String motivo = bloqueoAutomatico();
      if (motivo.length()) {
        evento(String("No riego ") + NOMBRES_VALVULAS[i] + ": " + motivo);
        continue;
      }
      uint8_t minutos = pr.duracion;
      if (modoInteligente && ajusteVigente()) {
        uint8_t aj = ajusteZona[i];
        if (aj == 0) {
          evento(String("No riego ") + NOMBRES_VALVULAS[i] + ": riego inteligente (0% hoy)");
          continue;
        }
        minutos = (uint8_t)constrain((int)pr.duracion * aj / 100, 1, (int)MAX_PROG_MIN);
        evento(String(NOMBRES_VALVULAS[i]) + ": " + pr.duracion + " min x " + aj + "% = " + minutos + " min");
      }
      encolarRiego(i, minutos);
    }
  }
}

// =====================================================================
//  Programas <-> JSON
// =====================================================================
void publicarProgramas() {
  if (!mqtt.connected()) return;
  JsonDocument doc;
  JsonObject zonas = doc["zonas"].to<JsonObject>();
  for (int i = 1; i < NUM_RELES; i++) {
    JsonArray arr = zonas[String(i + 1)].to<JsonArray>();
    for (int p = 0; p < 3; p++) {
      JsonObject o = arr.add<JsonObject>();
      o["h"] = horarios[i][p].hora;
      o["m"] = horarios[i][p].minuto;
      o["min"] = horarios[i][p].duracion;
      o["dias"] = horarios[i][p].dias;
    }
  }
  static char out[1800];
  size_t n = serializeJson(doc, out, sizeof(out));
  mqtt.publish("riego/s3/programas", (const uint8_t*)out, n, true);
}

void refrescarEditor();
void refrescarUI();

bool aplicarProgramasJson(const char* json, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) { evento("Programas recibidos con formato incorrecto"); return false; }
  JsonObject zonas = doc["zonas"];
  if (zonas.isNull()) { evento("Programas recibidos sin 'zonas'"); return false; }
  int cambios = 0;
  for (JsonPair kv : zonas) {
    int idx = atoi(kv.key().c_str()) - 1;     // "2" -> indice 1
    if (idx < 1 || idx >= NUM_RELES) continue;
    JsonArray arr = kv.value().as<JsonArray>();
    for (int p = 0; p < 3; p++) {
      ProgramaRiego nuevo = {6, 0, 0, 0x7F};
      if (p < (int)arr.size()) {
        int h = arr[p]["h"] | 6, m = arr[p]["m"] | 0, d = arr[p]["min"] | 0, dias = arr[p]["dias"] | 0x7F;
        if (h < 0 || h > 23 || m < 0 || m > 59 || d < 0) continue;
        if (d > (int)MAX_PROG_MIN) d = MAX_PROG_MIN;
        nuevo = {(uint8_t)h, (uint8_t)m, (uint8_t)d, (uint8_t)(dias & 0x7F)};
      }
      horarios[idx][p] = nuevo;
      cambios++;
    }
  }
  if (cambios) {
    guardarProgramas();
    publicarProgramas();
    refrescarEditor();
    evento("Programas actualizados desde el servidor");
  }
  return cambios > 0;
}

void setInteligente(bool on, const char* origen) {
  modoInteligente = on;
  prefs.putBool("intel", on);
  if (switch_inteligente) {
    if (on) lv_obj_add_state(switch_inteligente, LV_STATE_CHECKED);
    else    lv_obj_clear_state(switch_inteligente, LV_STATE_CHECKED);
  }
  if (mqtt.connected()) mqtt.publish("riego/s3/inteligente", on ? "ON" : "OFF", true);
  evento(String("Riego inteligente ") + (on ? "ACTIVADO" : "DESACTIVADO") + " desde " + origen);
}

// =====================================================================
//  Pantalla
// =====================================================================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.drawRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t * indev_driver, lv_indev_data_t * data) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    data->point.x = map(p.x, 200, 3800, 0, 320);
    data->point.y = map(p.y, 200, 3800, 0, 240);
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

static void event_switch_valvula(lv_event_t * e) {
  lv_obj_t * sw = lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

  if (idx == VALVULA_REMOTA_IDX) {
    // Entrada calle: solo mandamos la orden; el estado real lo confirma la DevKit
    if (mqtt.connected()) {
      mqtt.publish("riego/valvula/9/set", on ? "ON" : "OFF");
    } else {
      actualizarSwitch(idx, !on);   // sin MQTT no hay orden: deshacemos
    }
    return;
  }
  if (idx == IDX_ENTRADA) { setEntrada(on, "pantalla"); return; }
  if (on) iniciarRiego(idx, MAX_MANUAL_MIN, ORIG_MANUAL);
  else    pararRiego(idx, "manual (pantalla)");
}

// Interruptor de cada sensor (pestana Sensores): activa/desactiva el sensor y sus avisos.
// Se guarda en la placa y se publica retenido para que el servidor y Jarvis lo sepan.
void setSensor(int i, bool activo, bool publicar) {
  if (i < 0 || i > 7) return;
  uint8_t antes = sensorMask;
  if (activo) sensorMask |=  (1 << i);
  else        sensorMask &= ~(1 << i);
  if (sensorMask != antes) prefs.putUChar("sens", sensorMask);
  if (switch_sensores[i]) {
    if (activo) lv_obj_add_state(switch_sensores[i], LV_STATE_CHECKED);
    else        lv_obj_clear_state(switch_sensores[i], LV_STATE_CHECKED);
  }
  if (publicar && mqtt.connected()) {
    char topic[32];
    snprintf(topic, sizeof(topic), "riego/sensor/%d/activo", i + 1);
    mqtt.publish(topic, activo ? "1" : "0", true);
  }
  if (sensorMask != antes) evento(String("Sensor N") + (i + 1) + (activo ? " ACTIVADO" : " DESACTIVADO (sin avisos)"));
}

static void event_switch_sensor(lv_event_t * e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  setSensor(i, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED), true);
  refrescarUI();
}

static void event_switch_inteligente(lv_event_t * e) {
  setInteligente(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED), "pantalla");
}

static void event_guardar_prog(lv_event_t * e) {
  int zona = lv_dropdown_get_selected(dd_zonas) + 1;   // indice 1..7 (V2..V8)
  int p    = lv_dropdown_get_selected(dd_progs);
  horarios[zona][p].hora     = lv_slider_get_value(slider_hora);
  horarios[zona][p].minuto   = lv_slider_get_value(slider_min);
  horarios[zona][p].duracion = lv_slider_get_value(slider_duracion);
  if (horarios[zona][p].dias == 0) horarios[zona][p].dias = 0x7F;
  guardarProgramas();
  publicarProgramas();
  evento(String("Programa ") + (p + 1) + " de " + NOMBRES_VALVULAS[zona] + " guardado desde la pantalla");
}

static void event_cargar_prog(lv_event_t * e) {
  if (dd_zonas == NULL || dd_progs == NULL || slider_hora == NULL) return;
  int zona = lv_dropdown_get_selected(dd_zonas) + 1;
  int p    = lv_dropdown_get_selected(dd_progs);
  lv_slider_set_value(slider_hora, horarios[zona][p].hora, LV_ANIM_OFF);
  lv_slider_set_value(slider_min, horarios[zona][p].minuto, LV_ANIM_OFF);
  lv_slider_set_value(slider_duracion, horarios[zona][p].duracion, LV_ANIM_OFF);
  lv_event_send(slider_hora, LV_EVENT_VALUE_CHANGED, NULL);
  lv_event_send(slider_min, LV_EVENT_VALUE_CHANGED, NULL);
  lv_event_send(slider_duracion, LV_EVENT_VALUE_CHANGED, NULL);
}

void refrescarEditor() { if (dd_zonas) lv_event_send(dd_zonas, LV_EVENT_VALUE_CHANGED, NULL); }

static void event_slider_hora(lv_event_t * e) {
  if (label_hora_sel != NULL) {
    char b[20];
    snprintf(b, sizeof(b), "Hora: %d", (int)lv_slider_get_value(slider_hora));
    lv_label_set_text(label_hora_sel, b);
  }
}
static void event_slider_min(lv_event_t * e) {
  if (label_min_sel != NULL) {
    char b[20];
    snprintf(b, sizeof(b), "Minuto: %02d", (int)lv_slider_get_value(slider_min));
    lv_label_set_text(label_min_sel, b);
  }
}
static void event_slider_dur(lv_event_t * e) {
  if (label_dur_sel != NULL) {
    char b[24];
    snprintf(b, sizeof(b), "Duracion: %d min", (int)lv_slider_get_value(slider_duracion));
    lv_label_set_text(label_dur_sel, b);
  }
}

void crearUI() {
  lv_obj_t * tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 38);
  lv_obj_t * tab_sensores = lv_tabview_add_tab(tabview, "Sensores");
  lv_obj_t * tab_valvulas = lv_tabview_add_tab(tabview, "Manual");
  lv_obj_t * tab_riego    = lv_tabview_add_tab(tabview, "RainBird");

  // ---- Sensores ----
  lv_obj_t * page_sensores = lv_obj_create(tab_sensores);
  lv_obj_set_size(page_sensores, 305, 185);
  lv_obj_align(page_sensores, LV_ALIGN_CENTER, 0, 5);
  lv_obj_set_style_border_width(page_sensores, 0, 0);
  lv_obj_set_style_pad_all(page_sensores, 0, 0);

  lv_obj_t * cont_meteo = lv_obj_create(page_sensores);
  lv_obj_set_size(cont_meteo, 280, 75);
  lv_obj_align(cont_meteo, LV_ALIGN_TOP_MID, 0, 0);

  label_meteo = lv_label_create(cont_meteo);
  lv_label_set_text(label_meteo, "DHT11: -- C | -- %\nLluvia: Comprobando...");
  lv_obj_align(label_meteo, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t * lbl_dep = lv_label_create(cont_meteo);
  lv_label_set_text(lbl_dep, "Deposito:");
  lv_obj_align(lbl_dep, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  bar_deposito = lv_bar_create(cont_meteo);
  lv_obj_set_size(bar_deposito, 100, 12);
  lv_obj_align(bar_deposito, LV_ALIGN_BOTTOM_MID, 10, -2);
  lv_bar_set_range(bar_deposito, 0, 100);

  label_deposito_pct = lv_label_create(cont_meteo);
  lv_label_set_text(label_deposito_pct, "--%");
  lv_obj_align(label_deposito_pct, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

  for (int i = 0; i < 8; i++) {
    label_nodos[i] = lv_label_create(page_sensores);
    lv_label_set_text(label_nodos[i], "-");
    lv_obj_set_pos(label_nodos[i], 10, 87 + (i * 30));

    switch_sensores[i] = lv_switch_create(page_sensores);
    lv_obj_set_size(switch_sensores[i], 45, 24);
    lv_obj_set_pos(switch_sensores[i], 225, 83 + (i * 30));
    if (sensorActivo(i)) lv_obj_add_state(switch_sensores[i], LV_STATE_CHECKED);
    lv_obj_add_event_cb(switch_sensores[i], event_switch_sensor, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
  }

  // Version e IP de la placa, al final de la pestana Sensores
  label_version = lv_label_create(page_sensores);
  lv_label_set_text(label_version, FW_VERSION " | sin WiFi");
  lv_obj_set_pos(label_version, 10, 87 + (8 * 30));

  // ---- Manual ----
  for (int i = 0; i < 9; i++) {
    int col = i % 2;
    int fila = i / 2;
    lv_obj_t * lbl = lv_label_create(tab_valvulas);
    lv_label_set_text(lbl, NOMBRES_VALVULAS[i]);
    lv_obj_set_pos(lbl, col == 0 ? 10 : 160, 10 + (fila * 48));

    switch_valvulas[i] = lv_switch_create(tab_valvulas);
    lv_obj_set_size(switch_valvulas[i], 45, 24);
    lv_obj_set_pos(switch_valvulas[i], (col == 0 ? 10 : 160) + 95, 6 + (fila * 48));
    lv_obj_add_event_cb(switch_valvulas[i], event_switch_valvula, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);
  }

  // ---- Programas (RainBird) ----
  label_reloj = lv_label_create(tab_riego);
  lv_label_set_text(label_reloj, "Reloj: Sincronizando Wi-Fi...");
  lv_obj_align(label_reloj, LV_ALIGN_TOP_MID, 0, -10);

  dd_zonas = lv_dropdown_create(tab_riego);
  lv_dropdown_set_options(dd_zonas, "2. Arboles\n3. Huerto\n4. Parterre\n5. P. Superior\n6. Aux 6\n7. Aux 7\n8. Aux 8");
  lv_obj_set_size(dd_zonas, 115, 30);
  lv_obj_align(dd_zonas, LV_ALIGN_TOP_LEFT, 0, 15);

  dd_progs = lv_dropdown_create(tab_riego);
  lv_dropdown_set_options(dd_progs, "Prog 1\nProg 2\nProg 3");
  lv_obj_set_size(dd_progs, 115, 30);
  lv_obj_align(dd_progs, LV_ALIGN_TOP_LEFT, 0, 50);

  slider_hora = lv_slider_create(tab_riego);
  label_hora_sel = lv_label_create(tab_riego);
  lv_obj_set_size(slider_hora, 150, 8);
  lv_obj_align(slider_hora, LV_ALIGN_TOP_RIGHT, -5, 20);
  lv_obj_align_to(label_hora_sel, slider_hora, LV_ALIGN_OUT_TOP_MID, 0, -3);
  lv_slider_set_range(slider_hora, 0, 23);

  slider_min = lv_slider_create(tab_riego);
  label_min_sel = lv_label_create(tab_riego);
  lv_obj_set_size(slider_min, 150, 8);
  lv_obj_set_pos(slider_min, 150, 75);
  lv_obj_align_to(label_min_sel, slider_min, LV_ALIGN_OUT_TOP_MID, 0, -3);
  lv_slider_set_range(slider_min, 0, 59);

  slider_duracion = lv_slider_create(tab_riego);
  label_dur_sel = lv_label_create(tab_riego);
  lv_obj_set_size(slider_duracion, 150, 8);
  lv_obj_set_pos(slider_duracion, 150, 125);
  lv_obj_align_to(label_dur_sel, slider_duracion, LV_ALIGN_OUT_TOP_MID, 0, -3);
  lv_slider_set_range(slider_duracion, 0, 90);

  lv_obj_t * btn_guardar = lv_btn_create(tab_riego);
  lv_obj_align(btn_guardar, LV_ALIGN_BOTTOM_LEFT, 0, -5);
  lv_obj_t * lbl_btn = lv_label_create(btn_guardar);
  lv_label_set_text(lbl_btn, "Guardar Prog");

  lv_obj_t * lbl_intel = lv_label_create(tab_riego);
  lv_label_set_text(lbl_intel, "Inteligente");
  lv_obj_align(lbl_intel, LV_ALIGN_BOTTOM_RIGHT, -55, -9);
  switch_inteligente = lv_switch_create(tab_riego);
  lv_obj_set_size(switch_inteligente, 45, 24);
  lv_obj_align(switch_inteligente, LV_ALIGN_BOTTOM_RIGHT, -5, -5);

  lv_obj_add_event_cb(dd_zonas, event_cargar_prog, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(dd_progs, event_cargar_prog, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(slider_hora, event_slider_hora, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(slider_min, event_slider_min, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(slider_duracion, event_slider_dur, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(btn_guardar, event_guardar_prog, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(switch_inteligente, event_switch_inteligente, LV_EVENT_VALUE_CHANGED, NULL);

  refrescarEditor();
}

void refrescarUI() {
  if (label_meteo == NULL || bar_deposito == NULL || label_deposito_pct == NULL) return;

  String strTemp = isnan(tempAire) ? "--" : String(tempAire, 1);
  String strHum  = isnan(humAire)  ? "--" : String(humAire, 0);
  String strLluvia = lloviendo ? "SI (Bloqueado)" : "SECO";
  String textoMeteo = "DHT11: " + strTemp + " C | " + strHum + " %\nLluvia: " + strLluvia;
  lv_label_set_text(label_meteo, textoMeteo.c_str());

  if (nivelDeposito >= 0) {
    lv_bar_set_value(bar_deposito, nivelDeposito, LV_ANIM_OFF);
    String depTxt = String(nivelDeposito) + "%";
    lv_label_set_text(label_deposito_pct, depTxt.c_str());
  } else {
    lv_bar_set_value(bar_deposito, 0, LV_ANIM_OFF);
    lv_label_set_text(label_deposito_pct, "--%");
  }

  unsigned long ahora = millis();
  for (int i = 0; i < 8; i++) {
    if (label_nodos[i] == NULL) continue;
    String txt = "N" + String(i + 1) + ": ";
    if (!sensorActivo(i))                              txt += "DESACTIVADO";
    else if (ultimaVezVisto[i] == 0)                   txt += "ESPERANDO";
    else if (ahora - ultimaVezVisto[i] > TIMEOUT_NODO) txt += "ERR (SIN SENAL)";
    else                                               txt += String(humedadNodos[i]) + "%";
    lv_label_set_text(label_nodos[i], txt.c_str());
  }
}

// =====================================================================
//  MQTT
// =====================================================================
bool esOn(const String& m)  { return m.equalsIgnoreCase("ON") || m == "1" || m.equalsIgnoreCase("TRUE") || m.equalsIgnoreCase("SI"); }
bool esOff(const String& m) { return m.equalsIgnoreCase("OFF") || m == "0" || m.equalsIgnoreCase("FALSE") || m.equalsIgnoreCase("NO"); }

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);

  if (t == "riego/s3/ajuste/set") {          // retenido: tambien vale el que llega al conectar
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)payload, length)) return;
    time_t hasta = doc["valido_hasta"] | 0;
    JsonObject zonas = doc["zonas"];
    if (zonas.isNull() || hasta == 0) return;
    String resumen;
    for (JsonPair kv : zonas) {
      int idx = atoi(kv.key().c_str()) - 1;
      if (idx < 1 || idx >= NUM_RELES) continue;
      ajusteZona[idx] = (uint8_t)constrain(kv.value().as<int>(), 0, 250);
      resumen += String(" V") + (idx + 1) + "=" + ajusteZona[idx] + "%";
    }
    ajusteHasta = hasta;
    evento("Ajuste del riego inteligente recibido:" + resumen + (modoInteligente ? "" : " (inteligente apagado: no se aplica)"));
    return;
  }

  if (t == "riego/s3/programas/set") {
    aplicarProgramasJson((const char*)payload, length);
    return;
  }

  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  // Ordenes retenidas que llegan justo al conectar: se ignoran (no reabrir tras reinicio)
  bool recienConectado = millis() - tSuscrito < 1500;

  if (t.startsWith("riego/valvula/") && t.endsWith("/set")) {
    if (recienConectado) return;
    int id = t.substring(strlen("riego/valvula/"), t.length() - strlen("/set")).toInt();
    if (id == 1) {
      if (esOn(msg)) setEntrada(true, "remoto");
      else if (esOff(msg)) setEntrada(false, "remoto");
    } else if (id >= 2 && id <= 8) {
      int idx = id - 1;
      int minutos = msg.toInt();
      if (esOn(msg))        iniciarRiego(idx, MAX_MANUAL_MIN, ORIG_MANUAL);
      else if (minutos > 0) iniciarRiego(idx, minutos, ORIG_REMOTO);
      else if (esOff(msg))  pararRiego(idx, "orden remota");
    }
    return;
  }

  if (t == "riego/s3/inteligente/set") {
    if (recienConectado) return;
    if (esOn(msg)) setInteligente(true, "remoto");
    else if (esOff(msg)) setInteligente(false, "remoto");
    return;
  }

  if (t == "riego/valvula/9/estado") {
    actualizarSwitch(VALVULA_REMOTA_IDX, msg.startsWith("ON") || msg == "1" || msg.equalsIgnoreCase("true"));
    return;
  }

  if (t.startsWith("riego/sensores/nodo")) {
    int id = t.substring(strlen("riego/sensores/nodo")).toInt();
    if (id >= 1 && id <= 8) {
      humedadNodos[id - 1] = constrain(msg.toInt(), 0, 100);
      ultimaVezVisto[id - 1] = millis();
      refrescarUI();
    }
    return;
  }

  // riego/sensor/N/activo  "1" / "0"
  if (t.startsWith("riego/sensor/") && t.endsWith("/activo")) {
    // Al conectar manda la placa: ignoro lo que el broker tuviera guardado y luego publico lo mio
    if (recienConectado) return;
    int id = t.substring(strlen("riego/sensor/"), t.length() - strlen("/activo")).toInt();
    if (id >= 1 && id <= 8) {
      if (esOn(msg))       setSensor(id - 1, true, false);
      else if (esOff(msg)) setSensor(id - 1, false, false);
      refrescarUI();
    }
    return;
  }

  if (t == "riego/clima/lluvia") {
    bool antes = lloviendo;
    lloviendo = esOn(msg);
    if (lloviendo && !antes && (hayRiegoAuto() || nCola > 0)) {
      vaciarCola();
      for (int i = 1; i < NUM_RELES; i++)
        if (riegos[i].activo && riegos[i].origen == ORIG_AUTO) pararRiego(i, "empieza a llover");
    }
    refrescarUI();
    return;
  }

  if (t == "riego/deposito/nivel") {
    nivelDeposito = constrain(msg.toInt(), 0, 100);
    tNivel = millis();
    refrescarUI();
    return;
  }
}

void resolverBroker() {
  String origen = "IP fija";
  IPAddress ip;
  ip.fromString(ipBrokerFija());
  uint16_t port = MQTT_FALLBACK_PORT;
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
  brokerIP = ip;
  brokerPort = port;
  brokerOrigen = origen + " (" + ip.toString() + ":" + port + ")";
  mqtt.setServer(brokerIP, brokerPort);
  Serial.println("[MQTT] Broker: " + brokerOrigen);
}

void publicarInfo() {
  if (!mqtt.connected()) return;
  char b[400];
  snprintf(b, sizeof(b),
    "{\"version\":\"%s\",\"uptime_s\":%lu,\"ip\":\"%s\",\"rssi\":%d,\"broker\":\"%s\","
    "\"arranque\":\"%s\",\"inteligente\":%s,\"entrada_abierta\":%s,\"lluvia\":%s,"
    "\"deposito\":%d,\"zonas_regando\":%d,\"en_cola\":%d,\"sensores_activos\":%u}",
    FW_VERSION, millis() / 1000, WiFi.localIP().toString().c_str(), WiFi.RSSI(),
    brokerOrigen.c_str(), motivoArranque.c_str(),
    modoInteligente ? "true" : "false", entradaCerrada ? "false" : "true",
    lloviendo ? "true" : "false", nivelDeposito,
    zonasRegando(),
    nCola, sensorMask);
  mqtt.publish("riego/s3/info", b, true);
}

void publicarTodo() {
  mqtt.publish("riego/s3/online", "1", true);
  publicarEstadoValvula(IDX_ENTRADA, entradaCerrada ? "OFF" : "ON");
  for (int i = 1; i < NUM_RELES; i++)
    publicarEstadoValvula(i, riegos[i].activo ? (riegos[i].origen == ORIG_AUTO ? "ON (AUTO)" : "ON") : "OFF");
  mqtt.publish("riego/s3/inteligente", modoInteligente ? "ON" : "OFF", true);
  for (int i = 0; i < 8; i++) {   // estado real de los interruptores de sensores
    char topic[32];
    snprintf(topic, sizeof(topic), "riego/sensor/%d/activo", i + 1);
    mqtt.publish(topic, sensorActivo(i) ? "1" : "0", true);
  }
  publicarProgramas();
  publicarInfo();
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
  bool conUsuario = strlen(mqtt_user) > 0;
  bool ok = mqtt.connect("ESPRiegoS3",
                         conUsuario ? mqtt_user : NULL, conUsuario ? mqtt_pass : NULL,
                         "riego/s3/online", 0, true, "0");
  alimentarVigilante();
  if (ok) {
    Serial.println(" OK");
    fallosMqtt = 0;
    rondaBroker = 0;
    if (mdnsDiag.length()) { mqtt.publish("riego/s3/evento", ("Broker: " + brokerOrigen + " | " + mdnsDiag).substring(0, 200).c_str()); mdnsDiag = ""; }
    mqtt.subscribe("riego/valvula/+/set");
    mqtt.subscribe("riego/valvula/9/estado");
    mqtt.subscribe("riego/sensores/+");
    mqtt.subscribe("riego/sensor/+/activo");
    mqtt.subscribe("riego/clima/lluvia");
    mqtt.subscribe("riego/deposito/nivel");
    mqtt.subscribe("riego/s3/programas/set");
    mqtt.subscribe("riego/s3/inteligente/set");
    mqtt.subscribe("riego/s3/ajuste/set");
    tSuscrito = millis();
    publicarTodo();
  } else {
    fallosMqtt++;
    Serial.print(" FALLO, rc=");
    Serial.println(mqtt.state());
  }
}

// =====================================================================
//  WiFi y OTA
// =====================================================================
void iniciarOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    pararTodasLasZonas("actualizacion de firmware");
    evento("Actualizando firmware por WiFi...");
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { alimentarVigilante(); });
  ArduinoOTA.onError([](ota_error_t err) { evento(String("Error en la actualizacion: ") + (int)err); });
  ArduinoOTA.begin();
  otaIniciada = true;
  Serial.println("[OTA] Lista como " HOSTNAME);
}

void conectarWiFi() {
  static bool avisado = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!avisado) {
      Serial.printf("WiFi conectado, IP: %s, senal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      avisado = true;
      if (!otaIniciada) iniciarOTA();
      if (label_version) {
        String v = String(FW_VERSION) + " | " + WiFi.localIP().toString();
        lv_label_set_text(label_version, v.c_str());
      }
    }
    tWifiInicio = millis();
    return;
  }
  avisado = false;
  // Con autoReconnect el ESP32 reintenta solo. Si en 60 s no lo consigue, reinicio el WiFi.
  if (millis() - tWifiInicio > 60000) {
    Serial.println("WiFi: 60 s sin conexion, reinicio la conexion");
    WiFi.disconnect();
    empezarWiFi();
    tWifiInicio = millis();
  }
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

// =====================================================================
//  SETUP / LOOP
// =====================================================================
unsigned long tReloj = 0;
unsigned long tDht = 0;
unsigned long tInfo = 0;
int ultimoMinutoRevisado = -1;

void setup() {
  Serial.begin(115200);
  motivoArranque = calcularMotivoArranque();

  // 1) Estado guardado y reles ANTES que nada
  cargarMemoria();
  for (int i = 0; i < NUM_RELES; i++) {
    pinMode(PINES_RELES[i], OUTPUT);
    aplicarSalida(i, i == IDX_ENTRADA ? !entradaCerrada : false);
  }

  // 2) Vigilante: si loop() deja de ejecutarse 60 s, reinicio
  wdt = timerBegin(0, 80, true);                        // 1 tick = 1 us
  timerAttachInterrupt(wdt, &reiniciarPorBloqueo, true);
  timerAlarmWrite(wdt, (uint64_t)WDT_SEGUNDOS * 1000000ULL, false);
  timerAlarmEnable(wdt);

  // 3) Firmware recien actualizado? Hay que confirmarlo o vuelve al anterior
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t estadoOta;
  if (esp_ota_get_state_partition(running, &estadoOta) == ESP_OK && estadoOta == ESP_OTA_IMG_PENDING_VERIFY) {
    pendienteVerificar = true;
  }

  Serial.printf("\n%s | arranque: %s | heap libre: %u\n", FW_VERSION, motivoArranque.c_str(), ESP.getFreeHeap());

  // 4) Pantalla
  SPI.begin(SCLK_PIN, MISO_PIN, MOSI_PIN, -1);
  tft.begin();
  tft.setRotation(1);
  ts.begin();
  ts.setRotation(1);
  dht.begin();

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 10);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  crearUI();
  actualizarSwitch(IDX_ENTRADA, !entradaCerrada);
  actualizarSwitch(VALVULA_REMOTA_IDX, true);   // entrada calle: normalmente abierta
  if (modoInteligente) lv_obj_add_state(switch_inteligente, LV_STATE_CHECKED);
  refrescarUI();

  // 5) Red
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  empezarWiFi();
  tWifiInicio = millis();

  mqtt.setBufferSize(2048);
  mqtt.setKeepAlive(60);   // con 15 s se caia en cortes cortos del WiFi (como el DevKit y el puente)
  mqtt.setCallback(mqttCallback);

  configTime(0, 0, ntpServer);
  setenv("TZ", tz_info, 1);
  tzset();
}

void loop() {
  alimentarVigilante();
  lv_timer_handler();

  conectarWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    conectarMQTT();
    if (otaIniciada && !hayRiegoActivo()) ArduinoOTA.handle();   // nunca actualizar regando
  }

  // Firmware nuevo: si conecta al WiFi, queda confirmado; si a los 3 min no, vuelve al anterior
  if (pendienteVerificar) {
    if (WiFi.status() == WL_CONNECTED && millis() > 60000) {
      esp_ota_mark_app_valid_cancel_rollback();
      pendienteVerificar = false;
      evento("Firmware nuevo confirmado: " FW_VERSION);
    } else if (millis() > 180000) {
      Serial.println("[OTA] El firmware nuevo no conecta: vuelvo a la version anterior");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }

  struct tm timeinfo;
  bool horaDisponible = getLocalTime(&timeinfo, 10);

  if (horaDisponible && millis() - tReloj > 1000) {
    char bufR[30];
    snprintf(bufR, sizeof(bufR), "Reloj: %02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    if (label_reloj != NULL) lv_label_set_text(label_reloj, bufR);
    tReloj = millis();
  }

  if (millis() - tDht > 5000) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      tempAire = t;
      humAire = h;
      // La pantalla se actualiza cada 5 s; al broker solo cada 60 s
      static unsigned long tDhtPub = 0;
      if (mqtt.connected() && (tDhtPub == 0 || millis() - tDhtPub > 60000)) {
        mqtt.publish("riego/clima/temperatura", String(t, 1).c_str());
        mqtt.publish("riego/clima/humedad", String(h, 0).c_str());
        tDhtPub = millis();
      }
    } else {
      tempAire = NAN;
      humAire = NAN;
    }
    refrescarUI();
    tDht = millis();
  }

  if (horaDisponible && timeinfo.tm_min != ultimoMinutoRevisado) {
    ultimoMinutoRevisado = timeinfo.tm_min;
    revisarProgramas(timeinfo);
  }

  revisarTemporizadores();

  if (millis() - tInfo > 60000) {
    publicarInfo();
    tInfo = millis();
  }

  delay(5);
}
