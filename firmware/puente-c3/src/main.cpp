// =====================================================================
//  PUENTE DE RADIO - ESP32-C3   v5.6.0
// =====================================================================
//  Base: v4 (RF original con pulseIn + fix de potencia TX), que funciona.
//
//  Hace dos trabajos:
//   1) Nodo de humedad N1 local (sonda propia, sin pasar por radio).
//   2) Puente RF para los nodos N2-N5 (protocolo casero de 16 bits).
//
//  NOVEDADES v5
//   - Actualizacion por WiFi (OTA) con contrasena: "riego-puente".
//   - Vuelta atras automatica si el firmware nuevo no conecta al WiFi.
//   - Vigilante: si el programa se cuelga 60 s, la placa se reinicia.
//   - Busca el broker solo en la red (mDNS "riego"); si no, IP fija.
//   - Aviso de caida (riego/puente/online) e informe cada 60 s.
//   - WiFi: un solo WiFi.begin con reconexion automatica.
//
//  NOVEDADES v5.2
//   - Recibe los nodos N2-N5 por ESP-NOW (via principal) y por radio 433
//     (respaldo). Responde a los nodos que lo buscan (HOLA) para que sepan
//     su direccion y canal. Publica por donde llego cada dato en
//     riego/puente/via/nodoX ("espnow" o "radio").
//   - La radio solo se escucha con MQTT conectado (arranque limpio).
//
//  NOVEDADES v5.6
//   - OTA de los nodos por ESP-NOW: orden MQTT TOPIC_BASE/ota_nodo
//     ("<nodo> <url> <tamano> <md5>"). Descarga el firmware del servidor a la
//     particion spiffs (libre) y lo manda a trozos al nodo (o a su repetidor),
//     esperando respuesta en cada trozo. Estado en TOPIC_BASE/ota_nodo/estado.
//   - Publica el MSG_INFO de los nodos en TOPIC_BASE/info_nodo/nodoN.
//
//  HARDWARE: la antena del receptor lleva un contrapeso de ~17 cm a GND.
// =====================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include "esp_wifi.h"
#include <esp_now.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_partition.h"
#include <HTTPClient.h>
#include <MD5Builder.h>

// --- WiFi / MQTT ---
#include "secretos.h"
#ifndef OTA_PASS
#error "Falta OTA_PASS en include/secretos.h (mira include/secretos.example.h)"
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#define MQTT_PASS ""
#endif

#define FW_VERSION "puente-c3 5.6.0"
// ---- Variante de placa (se cambia desde platformio.ini) ----
//  Puente C3 principal: valores por defecto de abajo.
//  Nodo con WiFi (p. ej. nodo 2): hace de puente pequeno; ver [env:nodo2] en platformio.ini.
#ifndef HOSTNAME
#define HOSTNAME   "riego-puente"
#endif
#ifndef TOPIC_BASE
#define TOPIC_BASE "riego/puente"     // riego/puente/online, /info, /evento, /via/...
#endif
#ifndef CLIENT_ID
#define CLIENT_ID  "PuenteC3RF"
#endif
#ifndef RF_RX_ACTIVO
#define RF_RX_ACTIVO 1                // 1 = tiene receptor 433 (puente principal)
#endif
#ifndef TX_POWER
#define TX_POWER 44                   // potencia WiFi/ESP-NOW (4 = 1 dBm): 44 = 11 dBm, 60 = 15 dBm
#endif
#ifndef SONDA_SECO
#define SONDA_SECO   3780             // valor bruto de la sonda al aire (0 %) - nodo 1, calibrada 25-oct-2026
#endif
#ifndef SONDA_MOJADO
#define SONDA_MOJADO 1408             // valor bruto de la sonda en agua (100 %) - nodo 1, calibrada 25-oct-2026
#endif
#ifndef ESPNOW_LR
#define ESPNOW_LR 0                   // 1 = ESP-NOW en modo largo alcance (solo lo oyen equipos con LR)
#endif
#ifndef PIN_RF_TX
#define PIN_RF_TX -1                  // >=0: emisor 433 para mandar su propia lectura si no hay MQTT
#endif

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

// --- RF casero (nodos de humedad N2-N5) ---
#define PIN_RF_RX 4
#define MARCA_MIN 2000
#define BIT1_MIN  1000
#define BIT0_MIN   150
#define DEBUG_RF 0

// --- ESP-NOW (nodos N2-N5). Debe coincidir con nodo-humedad ---
#define RIEGO_MAGIA 0x52      // 'R'
#define MSG_HOLA    1         // nodo -> todos: busco al puente
#define MSG_PUENTE  2         // puente -> nodo: aqui estoy
#define MSG_ACK     4         // puente -> nodo: recibido el dato con este seq (por difusion)
#define MSG_DATO    3         // nodo -> puente: lectura de humedad
#define MSG_INFO    5         // nodo -> puente: version y motivo de arranque
#define MSG_OTA_INICIO 10     // puente -> nodo: tamano y md5 del firmware
#define MSG_OTA_TROZO  11     // puente -> nodo: un trozo en la posicion pos
#define MSG_OTA_FIN    12     // puente -> nodo: comprobar y reiniciar
#define MSG_OTA_RESP   13     // nodo -> puente: estado y posicion que espera
#define OTA_TROZO_MAX  200
#define OTA_OK          0
#define OTA_HECHO       4

typedef struct __attribute__((packed)) {
  uint8_t  magia;
  uint8_t  tipo;
  uint8_t  nodo;
  uint8_t  valor;
  uint16_t seq;
} MsgRiego;

typedef struct __attribute__((packed)) {
  uint8_t magia, tipo, nodo;
  uint32_t tam;
  uint8_t md5[16];
} MsgOtaInicio;

typedef struct __attribute__((packed)) {
  uint8_t magia, tipo, nodo;
  uint32_t pos;
  uint8_t largo;
  uint8_t datos[OTA_TROZO_MAX];
} MsgOtaTrozo;

typedef struct __attribute__((packed)) {
  uint8_t magia, tipo, nodo, estado;
  uint32_t pos;
} MsgOtaResp;

// Ultima direccion por la que llego cada nodo (el propio nodo o su repetidor)
uint8_t macNodo[9][6];
volatile bool macNodoValida[9] = {false};
// MSG_INFO pendiente de publicar
volatile bool infoNodoNueva = false;
volatile uint8_t infoNodo = 0;
volatile uint16_t infoNodoSeq = 0;
// Respuesta del nodo durante una OTA
volatile bool otaRespNueva = false;
volatile uint8_t otaRespNodo = 0, otaRespEstado = 0;
volatile uint32_t otaRespPos = 0;
// OTA en marcha
enum { OTA_LIBRE, OTA_DESCARGAR, OTA_EMPEZAR, OTA_TROZOS, OTA_TERMINAR } otaFase = OTA_LIBRE;
uint8_t otaNodo = 0;
uint32_t otaTam = 0, otaPos = 0;
char otaUrl[120] = "";
char otaMd5[33] = "";
int otaIntentos = 0, otaPorcentaje = -1;
unsigned long otaEnviado = 0, otaInicio = 0;
const esp_partition_t* otaParticion = NULL;
unsigned long tSuscrito = 0;
void alMensajeMqtt(char* topic, byte* payload, unsigned int len);

// Cola pequena: el callback de ESP-NOW guarda y el loop publica
#define COLA_ESPNOW 8
volatile uint8_t colaNodo[COLA_ESPNOW];
volatile uint8_t colaValor[COLA_ESPNOW];
volatile uint8_t colaIni = 0, colaFin = 0;
unsigned long datosEspNow = 0;
bool espNowIniciado = false;
// Diagnostico ESP-NOW
volatile unsigned long holasRecibidos = 0, respuestasEnviadas = 0, respuestasFallidas = 0, mensajesRaros = 0;
volatile unsigned long respuestasEntregadas = 0, respuestasNoEntregadas = 0;
volatile int ultimoErrorRespuesta = 0;
volatile unsigned long balizasEnviadas = 0, balizasFallidas = 0;
volatile unsigned long desconexionesWiFi = 0;
volatile unsigned long tUltimaDesconexion = 0;
unsigned long holasMostrados = 0;

// Las respuestas a los HOLA se mandan desde una tarea propia, no desde el
// callback de recepcion (que corre dentro de la tarea del WiFi).
typedef struct { uint8_t mac[6]; uint8_t nodo; uint8_t tipo; uint16_t seq; } PeticionHola;
QueueHandle_t colaHolas = NULL;
const uint8_t DIFUSION_ESPNOW[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- Sonda de humedad LOCAL (actua como Nodo 1) ---
#ifndef NODO_LOCAL_ID
#define NODO_LOCAL_ID 1
#endif
#ifndef PIN_SENSOR_TIERRA
#define PIN_SENSOR_TIERRA 1
#endif

int ultimoBrutoLocal = -1;      // ultimo valor bruto de la sonda local (para calibrar)
unsigned long tHumedadLocal = 0;
unsigned long tWifiInicio = 0;
unsigned long tMqttRetry = 0;
unsigned long tInfo = 0;
unsigned long tramasRecibidas = 0;
bool otaIniciada = false;
bool wifiAvisado = false;
bool brokerResuelto = false;
int fallosMqtt = 0;
String mdnsDiag = "";         // lo que ha visto la busqueda mDNS (se manda como evento)
int rondaBroker = 0;        // que broker toca probar (se pone a 0 al conectar)
String brokerOrigen = "";

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

// ---------- Conexiones ----------
void iniciarOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    if (mqtt.connected()) mqtt.publish(TOPIC_BASE "/evento", "Actualizando firmware por WiFi...");
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { alimentarVigilante(); });
  ArduinoOTA.begin();
  otaIniciada = true;
  Serial.println("[OTA] Lista como " HOSTNAME);
}

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiAvisado) {
      Serial.printf("WiFi OK: IP %s, senal %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
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
  mqtt.setCallback(alMensajeMqtt);
  mqtt.setBufferSize(512);   // el JSON de info no cabe en los 256 por defecto
  mqtt.setKeepAlive(60);     // 60 s: aguanta cortes cortos del WiFi sin desconectarse
  mqtt.setSocketTimeout(10);
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
  bool ok = mqtt.connect(CLIENT_ID,
                         conUsuario ? MQTT_USER : NULL, conUsuario ? MQTT_PASS : NULL,
                         TOPIC_BASE "/online", 0, true, "0");
  alimentarVigilante();
  if (ok) {
    Serial.println(" OK");
    fallosMqtt = 0;
    rondaBroker = 0;
    if (mdnsDiag.length()) { mqtt.publish(TOPIC_BASE "/evento", ("Broker: " + brokerOrigen + " | " + mdnsDiag).substring(0, 200).c_str()); mdnsDiag = ""; }
    mqtt.publish(TOPIC_BASE "/online", "1", true);
    mqtt.subscribe(TOPIC_BASE "/ota_nodo");
    tSuscrito = millis();
  } else {
    fallosMqtt++;
    Serial.print(" FALLO, rc=");
    Serial.println(mqtt.state());
  }
}

void publicarHumedad(int id, int val) {
  char topic[50];
  char payload[10];
  snprintf(topic, sizeof(topic), "riego/sensores/nodo%d", id);
  snprintf(payload, sizeof(payload), "%d", val);
  mqtt.publish(topic, payload);
  Serial.printf("[Nodo %d] -> %d%%\n", id, val);
}

void publicarInfo() {
  if (!mqtt.connected()) return;
  char b[512];
  snprintf(b, sizeof(b),
    "{\"version\":\"%s\",\"uptime_s\":%lu,\"ip\":\"%s\",\"rssi\":%d,\"broker\":\"%s\","
    "\"arranque\":\"%s\",\"tramas_rf\":%lu,\"datos_espnow\":%lu,\"canal\":%d,"
    "\"sonda_bruto\":%d,\"desc_wifi\":%lu,\"ult_desc_hace_s\":%ld,\"balizas\":%lu,\"balizas_err\":%lu,\"holas\":%lu,\"resp_ok\":%lu,\"resp_err\":%lu,\"ult_err\":%d,\"entregadas\":%lu,\"no_entregadas\":%lu,\"raros\":%lu}",
    FW_VERSION, millis() / 1000, WiFi.localIP().toString().c_str(), WiFi.RSSI(),
    brokerOrigen.c_str(), motivoArranque.c_str(), tramasRecibidas, datosEspNow, (int)WiFi.channel(),
    ultimoBrutoLocal, desconexionesWiFi, desconexionesWiFi ? (long)((millis() - tUltimaDesconexion) / 1000) : -1L,
    balizasEnviadas, balizasFallidas, holasRecibidos, respuestasEnviadas, respuestasFallidas, ultimoErrorRespuesta,
    respuestasEntregadas, respuestasNoEntregadas, mensajesRaros);
  mqtt.publish(TOPIC_BASE "/info", b, true);
}

void publicarVia(int id, const char* via) {
  char topic[40];
  snprintf(topic, sizeof(topic), TOPIC_BASE "/via/nodo%d", id);
  mqtt.publish(topic, via, true);
}

// ---------- ESP-NOW ----------
// Se ejecuta en la tarea del WiFi: solo cosas rapidas.
void alRecibirEspNow(const uint8_t *mac, const uint8_t *datos, int len) {
  if (len >= (int)sizeof(MsgOtaResp) && datos[0] == RIEGO_MAGIA && datos[1] == MSG_OTA_RESP) {
    const MsgOtaResp *r = (const MsgOtaResp *)datos;
    otaRespNodo = r->nodo;
    otaRespEstado = r->estado;
    otaRespPos = r->pos;
    otaRespNueva = true;
    return;
  }
  if (len != sizeof(MsgRiego)) { mensajesRaros++; return; }
  const MsgRiego *m = (const MsgRiego *)datos;
  if (m->magia != RIEGO_MAGIA || m->nodo < 2 || m->nodo > 5) { mensajesRaros++; return; }
  if (m->tipo == MSG_HOLA || m->tipo == MSG_DATO || m->tipo == MSG_INFO) {
    memcpy(macNodo[m->nodo], mac, 6);
    macNodoValida[m->nodo] = true;
  }
  if (m->tipo == MSG_INFO) {
    infoNodo = m->nodo;
    infoNodoSeq = m->seq;
    infoNodoNueva = true;
    return;
  }

  if (m->tipo == MSG_HOLA) {
    holasRecibidos++;
    // Un nodo me busca: la respuesta la manda tareaRespuestas
    if (colaHolas) {
      PeticionHola ph;
      memcpy(ph.mac, mac, 6);
      ph.nodo = m->nodo;
      ph.tipo = MSG_HOLA;
      ph.seq = 0;
      xQueueSend(colaHolas, &ph, 0);
    }
  } else if (m->tipo == MSG_DATO && m->valor <= 100) {
    // Confirmacion propia: en modo LR la automatica no llega al nodo
    if (colaHolas) {
      PeticionHola ph;
      memcpy(ph.mac, mac, 6);
      ph.nodo = m->nodo;
      ph.tipo = MSG_DATO;
      ph.seq = m->seq;
      xQueueSend(colaHolas, &ph, 0);
    }
    uint8_t sig = (colaFin + 1) % COLA_ESPNOW;
    if (sig != colaIni) {         // si la cola esta llena, se pierde (llegara la siguiente)
      colaNodo[colaFin] = m->nodo;
      colaValor[colaFin] = m->valor;
      colaFin = sig;
    }
  }
}

void anadirPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;              // 0 = el canal actual (el del router)
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

// Cada segundo emite una baliza "aqui estoy" por difusion en su canal (la del
// router); los nodos la escuchan para saber direccion y canal del puente.
// Ademas responde a los HOLA (compatibilidad), directo y por difusion.
void tareaRespuestas(void *) {
  PeticionHola ph;
  MsgRiego baliza = {RIEGO_MAGIA, MSG_PUENTE, 0, ESPNOW_LR, 0};   // nodo 0 = para todos; valor = 1 si tiene LR
  for (;;) {
    if (xQueueReceive(colaHolas, &ph, pdMS_TO_TICKS(1000)) != pdTRUE) {
      baliza.seq++;
      // bits 4-7 = canal real (cerca se oye el aviso tambien en canales vecinos)
      baliza.valor = (uint8_t)((ESPNOW_LR ? 1 : 0) | ((WiFi.channel() & 0x0F) << 4));
      if (esp_now_send(DIFUSION_ESPNOW, (uint8_t *)&baliza, sizeof(baliza)) == ESP_OK) balizasEnviadas++;
      else balizasFallidas++;
      continue;
    }
    if (ph.tipo == MSG_DATO) {
      MsgRiego ack = {RIEGO_MAGIA, MSG_ACK, ph.nodo, 0, ph.seq};
      esp_now_send(DIFUSION_ESPNOW, (uint8_t *)&ack, sizeof(ack));
      vTaskDelay(pdMS_TO_TICKS(5));
      esp_now_send(DIFUSION_ESPNOW, (uint8_t *)&ack, sizeof(ack));
      continue;
    }
    anadirPeer(ph.mac);
    MsgRiego r = {RIEGO_MAGIA, MSG_PUENTE, ph.nodo, (uint8_t)((ESPNOW_LR ? 1 : 0) | ((WiFi.channel() & 0x0F) << 4)), 0};
    for (int i = 0; i < 3; i++) {
      esp_err_t e1 = esp_now_send(ph.mac, (uint8_t *)&r, sizeof(r));
      esp_err_t e2 = esp_now_send(DIFUSION_ESPNOW, (uint8_t *)&r, sizeof(r));
      if (e1 == ESP_OK && e2 == ESP_OK) respuestasEnviadas++;
      else { respuestasFallidas++; ultimoErrorRespuesta = (int)(e1 != ESP_OK ? e1 : e2); }
      vTaskDelay(pdMS_TO_TICKS(15));
    }
  }
}

void alEnviarEspNow(const uint8_t *mac, esp_now_send_status_t estado) {
  if (memcmp(mac, DIFUSION_ESPNOW, 6) == 0) return;   // la difusion no se confirma
  if (estado == ESP_NOW_SEND_SUCCESS) respuestasEntregadas++;
  else respuestasNoEntregadas++;
}

void iniciarEspNow() {
  esp_err_t e = esp_now_init();
  if (e != ESP_OK) {
    Serial.printf("[ESP-NOW] No arranca (error %d); solo radio\n", (int)e);
    return;
  }
#if ESPNOW_LR
  {
    esp_err_t r = esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_LORA_250K);
    Serial.printf("[ESP-NOW] Largo alcance (LR 250k): %s\n", r == ESP_OK ? "OK" : esp_err_to_name(r));
  }
#endif
  esp_now_register_recv_cb(alRecibirEspNow);
  esp_now_register_send_cb(alEnviarEspNow);
  anadirPeer(DIFUSION_ESPNOW);
  colaHolas = xQueueCreate(8, sizeof(PeticionHola));
  xTaskCreate(tareaRespuestas, "espnow_resp", 3072, NULL, 2, NULL);
  Serial.printf("[ESP-NOW] MAC del puente: %s\n", WiFi.macAddress().c_str());
  espNowIniciado = true;
  Serial.println("[ESP-NOW] Escuchando nodos");
}

void atenderEspNow() {
  if (holasRecibidos != holasMostrados) {
    holasMostrados = holasRecibidos;
    Serial.printf("[ESP-NOW] HOLA=%lu resp_ok=%lu resp_err=%lu (ult=%d) entregadas=%lu no_entregadas=%lu raros=%lu canal=%d\n",
                  holasRecibidos, respuestasEnviadas, respuestasFallidas, ultimoErrorRespuesta,
                  respuestasEntregadas, respuestasNoEntregadas, mensajesRaros, (int)WiFi.channel());
  }
  while (colaIni != colaFin) {
    int id = colaNodo[colaIni];
    int val = colaValor[colaIni];
    colaIni = (colaIni + 1) % COLA_ESPNOW;
    datosEspNow++;
    if (mqtt.connected()) {
      publicarHumedad(id, val);
      publicarVia(id, "espnow");
    }
  }
}

// ---------- OTA de los nodos por ESP-NOW ----------
const char* MOTIVOS_NODO[] = {"?", "encendido", "bajada de tension", "vigilante (programa colgado)",
                              "fallo del programa", "watchdog del sistema", "reinicio por software"};

void publicarInfoNodo() {
  if (!infoNodoNueva || !mqtt.connected()) return;
  infoNodoNueva = false;
  uint8_t v = infoNodoSeq & 0xFF, mot = infoNodoSeq >> 8;
  char topic[48], b[96];
  snprintf(topic, sizeof(topic), TOPIC_BASE "/info_nodo/nodo%u", infoNodo);
  snprintf(b, sizeof(b), "{\"version\":\"%u.%u.%u\",\"arranque\":\"%s\"}",
           v / 100, (v / 10) % 10, v % 10, mot <= 6 ? MOTIVOS_NODO[mot] : "otro");
  mqtt.publish(topic, b, true);
}

void otaEstado(const String &texto) {
  Serial.println("[OTA nodo] " + texto);
  if (mqtt.connected()) mqtt.publish(TOPIC_BASE "/ota_nodo/estado", texto.c_str());
}

void otaFallo(const String &motivo) {
  otaEstado("error nodo " + String(otaNodo) + ": " + motivo);
  otaFase = OTA_LIBRE;
}

// Orden: "<nodo> <url> <tamano> <md5>"
void alMensajeMqtt(char* topic, byte* payload, unsigned int len) {
  if (millis() - tSuscrito < 1500) return;   // ordenes retenidas o viejas al conectar: fuera
  char b[220];
  len = len < sizeof(b) - 1 ? len : sizeof(b) - 1;
  memcpy(b, payload, len);
  b[len] = 0;
  unsigned int nodo; unsigned long tam;
  char url[120], md5[40];
  if (sscanf(b, "%u %119s %lu %39s", &nodo, url, &tam, md5) != 4 || nodo < 2 || nodo > 8 || strlen(md5) != 32) {
    otaEstado("error: orden no valida (nodo url tamano md5)");
    return;
  }
  if (otaFase != OTA_LIBRE) { otaEstado("error: ya hay una actualizacion en marcha"); return; }
  otaNodo = nodo;
  otaTam = tam;
  strncpy(otaUrl, url, sizeof(otaUrl));
  strncpy(otaMd5, md5, sizeof(otaMd5));
  for (char *c = otaMd5; *c; c++) *c = tolower(*c);
  otaFase = OTA_DESCARGAR;
}

// Baja el firmware del servidor a la particion spiffs (no se usa) y comprueba el md5
bool otaDescargar() {
  otaParticion = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
  if (!otaParticion || otaTam == 0 || otaTam > otaParticion->size) { otaFallo("no cabe en la particion"); return false; }
  otaEstado("descargando " + String(otaTam) + " bytes");
  uint32_t borrar = (otaTam + 4095) & ~4095UL;
  for (uint32_t d = 0; d < borrar; d += 65536) {
    uint32_t t = borrar - d < 65536 ? borrar - d : 65536;
    if (esp_partition_erase_range(otaParticion, d, t) != ESP_OK) { otaFallo("no puedo borrar la particion"); return false; }
    alimentarVigilante();
  }
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(otaUrl)) { otaFallo("url no valida"); return false; }
  int codigo = http.GET();
  if (codigo != 200) { http.end(); otaFallo("descarga HTTP " + String(codigo)); return false; }
  WiFiClient *flujo = http.getStreamPtr();
  MD5Builder md5;
  md5.begin();
  uint8_t buf[1024];
  uint32_t leido = 0;
  unsigned long tDato = millis();
  while (leido < otaTam) {
    alimentarVigilante();
    size_t hay = flujo->available();
    if (hay == 0) {
      if (!http.connected() || millis() - tDato > 15000) { http.end(); otaFallo("descarga cortada en " + String(leido)); return false; }
      delay(2);
      continue;
    }
    size_t n = flujo->readBytes(buf, min(hay, min(sizeof(buf), (size_t)(otaTam - leido))));
    if (esp_partition_write(otaParticion, leido, buf, n) != ESP_OK) { http.end(); otaFallo("no puedo escribir la particion"); return false; }
    md5.add(buf, n);
    leido += n;
    tDato = millis();
  }
  http.end();
  md5.calculate();
  if (md5.toString() != String(otaMd5)) { otaFallo("md5 de la descarga no coincide"); return false; }
  return true;
}

bool otaMandar(const uint8_t *datos, size_t len) {
  if (!macNodoValida[otaNodo]) return false;
  anadirPeer(macNodo[otaNodo]);
  esp_now_send(macNodo[otaNodo], datos, len);
  otaEnviado = millis();
  return true;
}

void otaMandarTrozo() {
  MsgOtaTrozo t;
  t.magia = RIEGO_MAGIA; t.tipo = MSG_OTA_TROZO; t.nodo = otaNodo; t.pos = otaPos;
  t.largo = (uint8_t)min((uint32_t)OTA_TROZO_MAX, otaTam - otaPos);
  esp_partition_read(otaParticion, otaPos, t.datos, t.largo);
  otaMandar((uint8_t *)&t, 8 + t.largo);
}

void otaMandarInicio() {
  MsgOtaInicio m;
  m.magia = RIEGO_MAGIA; m.tipo = MSG_OTA_INICIO; m.nodo = otaNodo; m.tam = otaTam;
  for (int i = 0; i < 16; i++) { char h[3] = {otaMd5[i * 2], otaMd5[i * 2 + 1], 0}; m.md5[i] = (uint8_t)strtoul(h, NULL, 16); }
  otaMandar((uint8_t *)&m, sizeof(m));
}

void otaMandarFin() {
  uint8_t f[3] = {RIEGO_MAGIA, MSG_OTA_FIN, otaNodo};
  otaMandar(f, sizeof(f));
}

// Un paso de la OTA en cada vuelta del loop (sin bloquear MQTT ni el vigilante)
void atenderOtaNodo() {
  if (otaFase == OTA_LIBRE) return;
  if (otaFase == OTA_DESCARGAR) {
    if (!macNodoValida[otaNodo]) { otaFallo("no conozco su direccion (espera a que mande un dato)"); return; }
    if (!otaDescargar()) return;
    otaEstado("descargado; empiezo a mandar al nodo " + String(otaNodo));
    otaFase = OTA_EMPEZAR; otaIntentos = 0; otaPos = 0; otaPorcentaje = -1; otaInicio = millis();
    otaRespNueva = false;
    otaMandarInicio();
    return;
  }
  bool hayResp = otaRespNueva && otaRespNodo == otaNodo;
  if (hayResp) {
    otaRespNueva = false;
    uint8_t est = otaRespEstado;
    if (est != OTA_OK && est != OTA_HECHO) { otaFallo("el nodo responde error " + String(est)); return; }
    otaIntentos = 0;
    if (est == OTA_HECHO) {
      otaEstado("ok nodo " + String(otaNodo) + ": firmware nuevo instalado en " + String((millis() - otaInicio) / 1000) + " s; reinicia");
      otaFase = OTA_LIBRE;
      return;
    }
    otaPos = otaRespPos;   // la posicion que espera el nodo manda
    if (otaFase == OTA_EMPEZAR) otaFase = OTA_TROZOS;
    if (otaPos >= otaTam) { otaFase = OTA_TERMINAR; otaMandarFin(); return; }
    otaFase = OTA_TROZOS;
    int pct = (int)((uint64_t)otaPos * 100 / otaTam);
    if (pct / 10 != otaPorcentaje / 10) { otaPorcentaje = pct; otaEstado("mandando al nodo " + String(otaNodo) + ": " + String(pct) + " %"); }
    otaMandarTrozo();
    return;
  }
  if (millis() - otaEnviado > 600) {   // sin respuesta: repito
    if (++otaIntentos > 40) { otaFallo("no responde (en " + String(otaPos) + " de " + String(otaTam) + ")"); return; }
    if (otaFase == OTA_EMPEZAR) otaMandarInicio();
    else if (otaFase == OTA_TERMINAR) otaMandarFin();
    else otaMandarTrozo();
  }
}

// Misma logica que custom_rx_C3.cpp (la que funcionaba).
bool leerTramaRF(uint16_t *valorOut) {
  unsigned long dStart = pulseIn(PIN_RF_RX, HIGH, 10000);
  if (dStart < MARCA_MIN) return false;

  uint16_t valor = 0;
  for (int i = 0; i < 16; i++) {
    unsigned long d = pulseIn(PIN_RF_RX, HIGH, 5000);
    bool bit1;
    if (d == 0) return false;
    else if (d >= BIT1_MIN && d < MARCA_MIN) bit1 = true;
    else if (d >= BIT0_MIN && d < BIT1_MIN) bit1 = false;
    else {
#if DEBUG_RF
      Serial.printf("[RF] Trama descartada en bit %d (%lu us)\n", i, d);
#endif
      return false;
    }
    valor = (valor << 1) | (bit1 ? 1 : 0);
  }

  unsigned long dStop = pulseIn(PIN_RF_RX, HIGH, 5000);
  if (dStop < MARCA_MIN) return false;

  *valorOut = valor;
  return true;
}

#if PIN_RF_TX >= 0
// Emisor 433 (mismo protocolo casero que los nodos): respaldo de la lectura propia
void pulsoRF(unsigned int us) {
  digitalWrite(PIN_RF_TX, HIGH); delayMicroseconds(us);
  digitalWrite(PIN_RF_TX, LOW);  delayMicroseconds(500);
}
void enviarPorRadio(uint16_t codigo) {
  for (int r = 0; r < 3; r++) {
    pulsoRF(3000);
    for (int i = 15; i >= 0; i--) pulsoRF(((codigo >> i) & 1) ? 1500 : 500);
    pulsoRF(3000);
    delay(150);
  }
}
#endif

// Lectura de la sonda: media de 16 muestras (quita el ruido del ADC y del WiFi)
int leerSondaBruto() {
  long suma = 0;
  for (int i = 0; i < 16; i++) { suma += analogRead(PIN_SENSOR_TIERRA); delay(2); }
  return (int)(suma / 16);
}
int brutoAPorcentaje(int bruto) {
  return constrain(map(bruto, SONDA_SECO, SONDA_MOJADO, 0, 100), 0, 100);
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
  Serial.begin(115200);
  delay(1000);
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

  analogReadResolution(12);
#if RF_RX_ACTIVO
  pinMode(PIN_RF_RX, INPUT);
#endif
#if PIN_RF_TX >= 0
  pinMode(PIN_RF_TX, OUTPUT);
  digitalWrite(PIN_RF_TX, LOW);
#endif

  Serial.printf("%s | arranque: %s\n", FW_VERSION, motivoArranque.c_str());

  WiFi.mode(WIFI_STA);
  delay(100);
#if ESPNOW_LR
  // Modo largo alcance de Espressif (solo entre ESP32); sigue pudiendo conectar al router
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
#endif
  esp_wifi_set_max_tx_power(TX_POWER);  // fix C3 SuperMini: 44 = 11dBm (antes 34 = 8.5dBm; ESP-NOW no llegaba a los nodos)
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);           // sin ahorro de energia: si no, se pierden mensajes ESP-NOW
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
    desconexionesWiFi++;
    tUltimaDesconexion = millis();
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  iniciarEspNow();
  empezarWiFi();
  tWifiInicio = millis();
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
      if (mqtt.connected()) mqtt.publish(TOPIC_BASE "/evento", "Firmware nuevo confirmado: " FW_VERSION);
    } else if (millis() > 180000) {
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }

  // --- Humedad LOCAL, se lee cada 5 s; al broker solo si cambia 3 % o mas, o cada 60 s ---
  static unsigned long tHumedadPub = 0;
  static int humedadPublicada = -100;
  if (millis() - tHumedadLocal > 5000) {
    int valorBruto = leerSondaBruto();
    ultimoBrutoLocal = valorBruto;
    int porcentaje = brutoAPorcentaje(valorBruto);
    if (mqtt.connected()) {
      if (abs(porcentaje - humedadPublicada) >= 3 || millis() - tHumedadPub > 60000) {
        publicarHumedad(NODO_LOCAL_ID, porcentaje);
        humedadPublicada = porcentaje;
        tHumedadPub = millis();
      }
    }
#if PIN_RF_TX >= 0
    else {
      enviarPorRadio((uint16_t)NODO_LOCAL_ID * 1000 + porcentaje);   // sin MQTT: al puente principal por radio
    }
#endif
    tHumedadLocal = millis();
  }

  // --- ESP-NOW: nodos remotos N2-N5 (via principal) ---
  atenderEspNow();
  publicarInfoNodo();
  atenderOtaNodo();

  // --- RF: nodos remotos N2-N5 (respaldo, solo con MQTT ya conectado) ---
#if RF_RX_ACTIVO
  uint16_t codigoRF;
  if (mqtt.connected() && leerTramaRF(&codigoRF)) {
    int id  = codigoRF / 1000;
    int val = codigoRF % 1000;
    if (id >= 2 && id <= 5 && val <= 100) {
      tramasRecibidas++;
      if (mqtt.connected()) { publicarHumedad(id, val); publicarVia(id, "radio"); }
    } else {
      Serial.printf("[RF] Fuera de rango (codigo=%u)\n", codigoRF);
    }
  }
#endif

  if (millis() - tInfo > 60000) {
    publicarInfo();
    tInfo = millis();
  }

  delay(2);
}
