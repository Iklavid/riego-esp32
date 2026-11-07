// NODO DE HUMEDAD (N2..N5) - v2.5.0
// Envio principal por ESP-NOW (con confirmacion de entrega) y,
// si falla, respaldo por la radio de 433 MHz (protocolo casero de siempre).
// Para otro nodo, cambia solo NODO_ID (2..5) y flashea.
//
// CABLEADO (ESP32-C3):
//   Radio (DATA del emisor) -> GPIO 2
//   Sonda de humedad (AOUT) -> GPIO 4   (VCC 3.3V, GND comun)
//
// ESP-NOW:
//   - Al arrancar busca el puente probando los canales 1..13 (mensaje HOLA por
//     difusion); el puente responde y el nodo se queda con su direccion y canal.
//     Asi no hace falta saber la MAC del puente ni el canal del router.
//   - Cada lectura se manda al puente y se espera su confirmacion (hasta 3 intentos).
//   - Si no confirma, esa lectura sale por radio 433.
//   - Tras 3 lecturas seguidas sin confirmacion, vuelve a buscar el puente
//     (como mucho una vez por minuto); mientras tanto, todo por radio.
//
// Radio 433 (respaldo), formato de la trama:
//   START: pulso HIGH de 3000us
//   16 bits (MSB primero): 500us = "0", 1500us = "1"
//   STOP:  pulso HIGH de 3000us
//   (pausa LOW de 500us entre cada pulso)
//   Payload: codigo = NODO_ID*1000 + porcentaje, enviado 3 veces seguidas.
//
// Vigilante: temporizador de 60 s alimentado en loop(); si se cuelga, reinicia.
// Cada 10 min manda MSG_INFO al puente: valor = 0 (libre), seq = motivo de arranque * 256 + version.
// No se buscan redes WiFi: la busqueda (scanNetworks) deja al nodo sin oir ESP-NOW (probado 07-11).
//
// OTA por ESP-NOW (v2.5): el puente con WiFi manda el firmware a trozos (MSG_OTA_*), cada
// trozo espera la respuesta del nodo. El repetidor pasa los trozos al nodo y las respuestas
// al puente. Si el firmware nuevo no encuentra el puente en 10 min, vuelve al anterior.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <Update.h>
#include "esp_ota_ops.h"

#define FW_VERSION "nodo-humedad 2.5.0"
#define FW_CODIGO  250          // version en numero, para MSG_INFO
// NODO_ID, ESPNOW_LR y MODO_REPETIDOR se ponen en cada env de platformio.ini
#ifndef NODO_ID
#define NODO_ID 3               // 2..5
#endif
#define TX_POWER 44             // potencia ESP-NOW (4 = 1 dBm): 44 = 11 dBm, 60 = 15 dBm, 84 = 21 dBm (maximo)
#ifndef ESPNOW_LR
#define ESPNOW_LR 1             // 1 = largo alcance: solo para nodos que se enganchan a un puente con LR (nodo 2)
#endif
#ifndef SONDA_SECO
#define SONDA_SECO   3245       // valor bruto de la sonda al aire (0 %); cada nodo la suya en platformio.ini
#endif
#ifndef SONDA_MOJADO
#define SONDA_MOJADO 1040       // valor bruto de la sonda en agua (100 %)
#endif
#ifndef MODO_REPETIDOR
#define MODO_REPETIDOR 0        // 1 = repetidor ESP-NOW sin sonda: recoge nodos lejanos y los reenvia a un puente con WiFi
#endif
#define PIN_TX 2
#define PIN_SENSOR_TIERRA 4

#define REPETICIONES 3
#define PAUSA_ENTRE_REPETICIONES_MS 150
#define PERIODO_LECTURA_MS 5000

#if ESPNOW_LR
#define ESCUCHA_CANAL_MS 3000       // en LR, escuchar mas para oir varios avisos del puente
#define ESPNOW_INTENTOS 5           // en LR el enlace va mas justo: mas intentos
#define ESPNOW_FALLOS_PARA_BUSCAR 6
#else
#define ESCUCHA_CANAL_MS 1200
#define ESPNOW_INTENTOS 3
#define ESPNOW_FALLOS_PARA_BUSCAR 3
#endif
#define BUSQUEDA_CADA_MS 60000UL
#define INFO_CADA_MS 600000UL     // MSG_INFO cada 10 min

// Marcas en el campo "valor" del aviso de un puente (MSG_PUENTE)
#define AVISO_LR         0x01     // el puente emite en largo alcance
#define AVISO_REPETIDOR  0x02     // es un repetidor (sin WiFi): un repetidor no se engancha a otro

// ---- Mensajes ESP-NOW (deben coincidir con el puente) ----
#define RIEGO_MAGIA 0x52      // 'R'
#define MSG_HOLA    1         // nodo -> todos: busco al puente
#define MSG_PUENTE  2         // puente -> nodo: aqui estoy
#define MSG_DATO    3         // nodo -> puente: lectura de humedad
#define MSG_ACK     4         // puente -> nodo: recibido (confirmacion propia, por difusion)
#define MSG_INFO    5         // nodo -> puente: version y motivo de arranque (sin confirmacion)

typedef struct __attribute__((packed)) {
  uint8_t  magia;
  uint8_t  tipo;
  uint8_t  nodo;
  uint8_t  valor;   // porcentaje 0..100
  uint16_t seq;
} MsgRiego;

// ---- OTA por ESP-NOW (deben coincidir con el puente) ----
#define MSG_OTA_INICIO 10     // puente -> nodo: tamano y md5 del firmware
#define MSG_OTA_TROZO  11     // puente -> nodo: un trozo en la posicion pos
#define MSG_OTA_FIN    12     // puente -> nodo: comprobar y reiniciar
#define MSG_OTA_RESP   13     // nodo -> puente: estado y posicion que espera
#define OTA_TROZO_MAX  200
#define OTA_OK          0
#define OTA_ERR_INICIO  1
#define OTA_ERR_ESCRIBIR 2
#define OTA_ERR_MD5     3
#define OTA_HECHO       4
#define OTA_SIN_INICIO  5

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

// El callback guarda los paquetes OTA y los atiende tareaOta (escribir en flash no cabe en el callback)
typedef struct { uint8_t mac[6]; uint8_t len; uint8_t datos[250]; } PaqueteOta;
QueueHandle_t colaOta = NULL;
bool otaEnCurso = false, otaHecho = false;
uint32_t otaTam = 0, otaPos = 0;

const uint8_t DIFUSION[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t macPuente[6];
uint8_t canalPuente = 0;          // 0 = puente no encontrado
uint8_t canalUltimo = 0;          // ultimo canal donde se oyo al puente
int fallosSeguidos = 0;
unsigned long tUltimaBusqueda = 0;
bool busquedaHecha = false;
uint16_t seq = 0;

volatile bool envioTerminado = false;
volatile bool envioOk = false;
volatile bool puenteRespondio = false;
volatile uint8_t canalAnunciado = 0;   // canal real que dice el aviso del puente (0 = no lo dice)
volatile uint8_t macRespuesta[6];
volatile unsigned long recibidosTotal = 0;
volatile uint16_t ackSeqRecibido = 0;
volatile uint8_t  ackNodoRecibido = 0;

// --- Vigilante ---
hw_timer_t* wdt = NULL;
RTC_NOINIT_ATTR uint32_t marcaBloqueo;
uint8_t motivoCodigo = 0;          // 1 encendido, 2 bajada de tension, 3 vigilante, 4 fallo, 5 watchdog, 6 software, 9 otro
unsigned long tUltimaInfo = 0;
bool infoPrimeraHecha = false;
bool pendienteVerificar = false;   // firmware nuevo por OTA sin confirmar
bool firmwareProbado = false;      // ya ha hablado con el puente: el firmware nuevo vale

extern "C" bool verifyRollbackLater() { return true; }

void IRAM_ATTR reiniciarPorBloqueo() {
  marcaBloqueo = 0xB10C;
  esp_restart();
}

inline void alimentarVigilante() { if (wdt) timerWrite(wdt, 0); }

const char* calcularMotivoArranque() {
  esp_reset_reason_t r = esp_reset_reason();
  const char* m;
  if (r == ESP_RST_SW && marcaBloqueo == 0xB10C) { m = "vigilante (programa colgado)"; motivoCodigo = 3; }
  else switch (r) {
    case ESP_RST_POWERON:  m = "encendido"; motivoCodigo = 1; break;
    case ESP_RST_SW:       m = "reinicio por software"; motivoCodigo = 6; break;
    case ESP_RST_PANIC:    m = "fallo del programa"; motivoCodigo = 4; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      m = "watchdog del sistema"; motivoCodigo = 5; break;
    case ESP_RST_BROWNOUT: m = "bajada de tension"; motivoCodigo = 2; break;
    default:               m = "otro"; motivoCodigo = 9; break;
  }
  marcaBloqueo = 0;
  return m;
}

#if MODO_REPETIDOR
// Datos de otros nodos pendientes de reenviar (los guarda el callback, los procesa el loop)
#define COLA_REP 8
volatile uint8_t  colaRepNodo[COLA_REP], colaRepValor[COLA_REP];
volatile uint16_t colaRepSeq[COLA_REP];
volatile uint8_t  colaRepIni = 0, colaRepFin = 0;
unsigned long reenviados = 0, reenviosFallidos = 0;
// MSG_INFO de otros nodos pendiente de reenviar (uno basta: llegan cada 10 min)
volatile bool infoRepPendiente = false;
volatile uint8_t infoRepNodo = 0, infoRepValor = 0;
volatile uint16_t infoRepSeq = 0;
// Direccion de cada nodo que reenvio (para pasarle los trozos de OTA)
uint8_t macNodo[9][6];
bool macNodoValida[9] = {false};
unsigned long otaReenviados = 0;
#endif   // diagnostico: todo lo que llega por ESP-NOW

// ---------- Radio 433 (respaldo) ----------
void enviarPulso(unsigned int duracionUs) {
  digitalWrite(PIN_TX, HIGH);
  delayMicroseconds(duracionUs);
  digitalWrite(PIN_TX, LOW);
  delayMicroseconds(500); // pausa entre pulsos
}

void enviarTrama(uint16_t valor) {
  enviarPulso(3000); // START
  for (int i = 15; i >= 0; i--) {
    bool bit1 = (valor >> i) & 0x01;
    enviarPulso(bit1 ? 1500 : 500);
  }
  enviarPulso(3000); // STOP
}

void enviarPorRadio(uint16_t codigo) {
  for (int i = 0; i < REPETICIONES; i++) {
    enviarTrama(codigo);
    delay(PAUSA_ENTRE_REPETICIONES_MS);
  }
}

// ---------- ESP-NOW ----------
void alEnviar(const uint8_t *mac, esp_now_send_status_t estado) {
  if (memcmp(mac, DIFUSION, 6) == 0) return;   // las difusiones (avisos, HOLA, ACK) no cuentan
  envioOk = (estado == ESP_NOW_SEND_SUCCESS);
  envioTerminado = true;
}

void alRecibir(const uint8_t *mac, const uint8_t *datos, int len) {
  recibidosTotal++;
  if (len >= 3 && len <= 250 && datos[0] == RIEGO_MAGIA && datos[1] >= MSG_OTA_INICIO && datos[1] <= MSG_OTA_RESP) {
    if (colaOta) {
      PaqueteOta p;
      memcpy(p.mac, mac, 6);
      p.len = len;
      memcpy(p.datos, datos, len);
      xQueueSend(colaOta, &p, 0);
    }
    return;
  }
  if (len != sizeof(MsgRiego)) return;
  const MsgRiego *m = (const MsgRiego *)datos;
  if (m->magia != RIEGO_MAGIA) return;
  if (m->tipo == MSG_ACK) {
    ackNodoRecibido = m->nodo;
    ackSeqRecibido = m->seq;
    return;
  }
#if MODO_REPETIDOR
  if (m->tipo == MSG_DATO && m->nodo >= 2 && m->nodo <= 8 && m->valor <= 100) {
    memcpy(macNodo[m->nodo], mac, 6);
    macNodoValida[m->nodo] = true;
    uint8_t sig = (colaRepFin + 1) % COLA_REP;
    if (sig != colaRepIni) {
      colaRepNodo[colaRepFin] = m->nodo;
      colaRepValor[colaRepFin] = m->valor;
      colaRepSeq[colaRepFin] = m->seq;
      colaRepFin = sig;
    }
    return;
  }
  if (m->tipo == MSG_INFO && m->nodo >= 2 && m->nodo <= 8 && m->nodo != NODO_ID) {
    infoRepNodo = m->nodo;
    infoRepValor = m->valor;
    infoRepSeq = m->seq;
    infoRepPendiente = true;
    return;
  }
  if (m->tipo == MSG_PUENTE && (m->valor & AVISO_REPETIDOR)) return;   // nunca engancharse a otro repetidor
#endif
  // nodo 0 = baliza del puente para todos; NODO_ID = respuesta a mi HOLA
  if (m->tipo != MSG_PUENTE || (m->nodo != 0 && m->nodo != NODO_ID)) return;
#if ESPNOW_LR
  if (!(m->valor & AVISO_LR)) return;   // con LR solo me engancho a puentes con LR (los demas no me oyen)
#endif
  for (int i = 0; i < 6; i++) macRespuesta[i] = mac[i];
  canalAnunciado = (m->valor >> 4) & 0x0F;
  puenteRespondio = true;
}

void ponerCanal(uint8_t canal) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(canal, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
}

void ponerPeer(const uint8_t *mac, uint8_t canal) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = canal;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

// Escucha un canal 1,2 s esperando la baliza del puente (la emite cada segundo).
bool probarCanal(uint8_t canal) {
  MsgRiego hola = {RIEGO_MAGIA, MSG_HOLA, NODO_ID, 0, 0};
  ponerCanal(canal);
  ponerPeer(DIFUSION, canal);
  puenteRespondio = false;
  unsigned long antes = recibidosTotal;
  esp_err_t ultimo = esp_now_send(DIFUSION, (uint8_t *)&hola, sizeof(hola));   // por si el puente es antiguo
  alimentarVigilante();
  unsigned long t0 = millis();
  while (!puenteRespondio && millis() - t0 < ESCUCHA_CANAL_MS) delay(5);
  Serial.printf("[ESP-NOW] canal %2u: envio=%s llegaron=%lu respuesta=%s\n", canal,
                ultimo == ESP_OK ? "OK" : esp_err_to_name(ultimo),
                recibidosTotal - antes, puenteRespondio ? "SI" : "no");
  if (!puenteRespondio) return false;
  for (int i = 0; i < 6; i++) macPuente[i] = macRespuesta[i];
  // Usar el canal que anuncia el puente, no el que escuchabamos: de cerca su aviso
  // se cuela tambien en los canales vecinos
  if (canalAnunciado >= 1 && canalAnunciado <= 13 && canalAnunciado != canal) {
    Serial.printf("[ESP-NOW] Oido en canal %u pero el puente esta en el %u\n", canal, canalAnunciado);
    canal = canalAnunciado;
    ponerCanal(canal);
    ponerPeer(DIFUSION, canal);   // la difusion (avisos/ACK) tambien en el canal bueno
  }
  canalPuente = canal;
  canalUltimo = canal;
  ponerPeer(macPuente, canal);
  Serial.printf("[ESP-NOW] Puente %02X:%02X:%02X:%02X:%02X:%02X en canal %u\n",
                macPuente[0], macPuente[1], macPuente[2], macPuente[3], macPuente[4], macPuente[5], canal);
  return true;
}

// Recorre los canales 1..13 (unos 15 s como mucho).
bool buscarPuente() {
  for (uint8_t canal = 1; canal <= 13; canal++) {
    if (probarCanal(canal)) return true;
  }
  canalPuente = 0;
  Serial.println("[ESP-NOW] Puente no encontrado; sigo por radio");
  return false;
}

// Manda un dato al puente y espera confirmacion (automatica o MSG_ACK del puente)
bool enviarDato(uint8_t nodo, uint8_t valor, uint16_t sq, int intentos, int esperaMs) {
  if (canalPuente == 0) return false;
  MsgRiego m = {RIEGO_MAGIA, MSG_DATO, nodo, valor, sq};
  for (int i = 0; i < intentos; i++) {
    envioTerminado = false;
    envioOk = false;
    if (esp_now_send(macPuente, (uint8_t *)&m, sizeof(m)) == ESP_OK) {
      // Vale la confirmacion automatica o la propia del puente (MSG_ACK con mi seq)
      unsigned long t0 = millis();
      while (millis() - t0 < (unsigned long)esperaMs) {
        if (envioTerminado && envioOk) return true;
        if (ackNodoRecibido == nodo && ackSeqRecibido == sq) return true;
        delay(1);
      }
    }
    delay(10 + random(20));
  }
  return false;
}

bool enviarPorEspNow(uint8_t porcentaje) {
  return enviarDato(NODO_ID, porcentaje, ++seq, ESPNOW_INTENTOS, 150);
}

// ---------- OTA por ESP-NOW ----------
void asegurarPeer(const uint8_t *mac) {
  if (!esp_now_is_peer_exist(mac)) ponerPeer(mac, canalPuente ? canalPuente : canalUltimo);
}

void responderOta(const uint8_t *mac, uint8_t estado) {
  MsgOtaResp r = {RIEGO_MAGIA, MSG_OTA_RESP, NODO_ID, estado, otaPos};
  asegurarPeer(mac);
  esp_now_send(mac, (uint8_t *)&r, sizeof(r));
}

void atenderOtaPropia(const PaqueteOta &p) {
  uint8_t tipo = p.datos[1];
  if (tipo == MSG_OTA_INICIO && p.len >= sizeof(MsgOtaInicio)) {
    MsgOtaInicio m;
    memcpy(&m, p.datos, sizeof(m));
    if (otaEnCurso && m.tam == otaTam && otaPos == 0) { responderOta(p.mac, OTA_OK); return; }  // repetido
    if (otaEnCurso) Update.abort();
    otaEnCurso = false;
    otaPos = 0;
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", m.md5[i]);
    if (!Update.begin(m.tam)) {
      Serial.printf("[OTA] No puedo empezar (%u bytes)\n", m.tam);
      responderOta(p.mac, OTA_ERR_INICIO);
      return;
    }
    Update.setMD5(hex);
    otaEnCurso = true;
    otaTam = m.tam;
    Serial.printf("[OTA] Empieza: %u bytes\n", m.tam);
    responderOta(p.mac, OTA_OK);
  } else if (tipo == MSG_OTA_TROZO && p.len >= 8) {
    if (!otaEnCurso) { responderOta(p.mac, otaHecho ? OTA_HECHO : OTA_SIN_INICIO); return; }
    MsgOtaTrozo m;
    memcpy(&m, p.datos, p.len < sizeof(m) ? p.len : sizeof(m));
    if (m.largo > OTA_TROZO_MAX || p.len < 8 + m.largo) return;
    if (m.pos == otaPos && otaPos + m.largo <= otaTam) {
      if (Update.write(m.datos, m.largo) != m.largo) {
        Serial.println("[OTA] Fallo al escribir");
        Update.abort();
        otaEnCurso = false;
        responderOta(p.mac, OTA_ERR_ESCRIBIR);
        return;
      }
      otaPos += m.largo;
    }
    responderOta(p.mac, OTA_OK);   // con la posicion que espero: el puente se pone al dia
  } else if (tipo == MSG_OTA_FIN) {
    if (!otaEnCurso) { responderOta(p.mac, otaHecho ? OTA_HECHO : OTA_SIN_INICIO); return; }
    if (otaPos < otaTam) { responderOta(p.mac, OTA_OK); return; }
    otaEnCurso = false;
    if (!Update.end()) {
      Serial.printf("[OTA] Firmware no valido (error %u)\n", Update.getError());
      responderOta(p.mac, OTA_ERR_MD5);
      return;
    }
    otaHecho = true;
    Serial.println("[OTA] Firmware nuevo correcto: reinicio");
    for (int i = 0; i < 3; i++) { responderOta(p.mac, OTA_HECHO); delay(30); }
    delay(500);
    ESP.restart();
  }
}

void tareaOta(void *) {
  PaqueteOta p;
  for (;;) {
    if (xQueueReceive(colaOta, &p, portMAX_DELAY) != pdTRUE) continue;
    uint8_t tipo = p.datos[1], nodo = p.datos[2];
#if MODO_REPETIDOR
    if (nodo != NODO_ID) {
      // No es para mi: trozos hacia el nodo, respuestas hacia el puente
      const uint8_t *destino = NULL;
      if (tipo == MSG_OTA_RESP) { if (canalPuente) destino = macPuente; }
      else if (nodo < 9 && macNodoValida[nodo]) destino = macNodo[nodo];
      if (destino) {
        asegurarPeer(destino);
        esp_now_send(destino, p.datos, p.len);
        otaReenviados++;
      }
      continue;
    }
#endif
    if (nodo != NODO_ID || tipo == MSG_OTA_RESP) continue;
    atenderOtaPropia(p);
  }
}

// Firmware nuevo por OTA: se confirma al hablar con el puente; si no, a los 10 min vuelve al anterior
void comprobarFirmwareNuevo() {
  if (!pendienteVerificar) return;
  if (firmwareProbado) {
    esp_ota_mark_app_valid_cancel_rollback();
    pendienteVerificar = false;
    Serial.println("[OTA] Firmware nuevo confirmado");
  } else if (millis() > 600000UL) {
    Serial.println("[OTA] El firmware nuevo no encuentra el puente: vuelvo al anterior");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

// MSG_INFO al puente cada 10 min (y en cuanto lo encuentra); sin confirmacion
void enviarInfo() {
  if (canalPuente == 0) return;
  if (infoPrimeraHecha && millis() - tUltimaInfo < INFO_CADA_MS) return;
  infoPrimeraHecha = true;
  tUltimaInfo = millis();
  MsgRiego m = {RIEGO_MAGIA, MSG_INFO, NODO_ID, 0, (uint16_t)((motivoCodigo << 8) | FW_CODIGO)};
  esp_now_send(macPuente, (uint8_t *)&m, sizeof(m));
  Serial.println("[INFO] enviado");
}

#if MODO_REPETIDOR
// Aviso "aqui estoy" del repetidor, en el canal de su puente
void emitirAviso() {
  MsgRiego a = {RIEGO_MAGIA, MSG_PUENTE, 0,
                (uint8_t)(AVISO_REPETIDOR | (ESPNOW_LR ? AVISO_LR : 0) | ((canalPuente & 0x0F) << 4)), 0};
  esp_now_send(DIFUSION, (uint8_t *)&a, sizeof(a));
}

// Reenvia lo recibido de otros nodos; solo confirma al nodo si el puente lo ha recibido,
// asi el nodo usa su radio de respaldo si la cadena falla.
void atenderRepetidor() {
  if (infoRepPendiente && canalPuente != 0) {
    MsgRiego m = {RIEGO_MAGIA, MSG_INFO, infoRepNodo, infoRepValor, infoRepSeq};
    infoRepPendiente = false;
    esp_now_send(macPuente, (uint8_t *)&m, sizeof(m));
  }
  while (colaRepIni != colaRepFin) {
    uint8_t nodo = colaRepNodo[colaRepIni];
    uint8_t valor = colaRepValor[colaRepIni];
    uint16_t sq = colaRepSeq[colaRepIni];
    colaRepIni = (colaRepIni + 1) % COLA_REP;
    if (enviarDato(nodo, valor, sq, 3, 40)) {
      reenviados++;
      fallosSeguidos = 0;
      MsgRiego ack = {RIEGO_MAGIA, MSG_ACK, nodo, 0, sq};
      esp_now_send(DIFUSION, (uint8_t *)&ack, sizeof(ack));
      delay(3);
      esp_now_send(DIFUSION, (uint8_t *)&ack, sizeof(ack));
      Serial.printf(">>> Reenviado nodo %u -> %u%% (seq %u)\n", nodo, valor, sq);
    } else {
      reenviosFallidos++;
      Serial.printf(">>> FALLO reenviando nodo %u (seq %u)\n", nodo, sq);
      if (canalPuente != 0 && ++fallosSeguidos >= ESPNOW_FALLOS_PARA_BUSCAR) {
        Serial.println("[ESP-NOW] Varios fallos seguidos: buscare el puente de nuevo");
        canalPuente = 0;
        fallosSeguidos = 0;
        tUltimaBusqueda = millis();
      }
    }
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

void setup() {
  Serial.begin(115200);
  delay(1000);
  const char* motivo = calcularMotivoArranque();

  wdt = timerBegin(0, 80, true);
  timerAttachInterrupt(wdt, &reiniciarPorBloqueo, true);
  timerAlarmWrite(wdt, 60ULL * 1000000ULL, false);
  timerAlarmEnable(wdt);

  const esp_partition_t* enUso = esp_ota_get_running_partition();
  esp_ota_img_states_t estadoOta;
  if (esp_ota_get_state_partition(enUso, &estadoOta) == ESP_OK && estadoOta == ESP_OTA_IMG_PENDING_VERIFY)
    pendienteVerificar = true;

  analogReadResolution(12);
  pinMode(PIN_TX, OUTPUT);
  digitalWrite(PIN_TX, LOW);

  WiFi.mode(WIFI_STA);
#if ESPNOW_LR
  // Modo largo alcance de Espressif (solo entre ESP32); sigue pudiendo conectar al router
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
#endif
  WiFi.setSleep(false);   // radio siempre escuchando (si no, puede perder la respuesta del puente)
  esp_wifi_set_max_tx_power(TX_POWER);   // C3 SuperMini: con su antena de chip, a mas potencia puede fallar
  WiFi.disconnect();
  if (esp_now_init() == ESP_OK) {
#if ESPNOW_LR
  {
    esp_err_t r = esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_LORA_250K);
    Serial.printf("[ESP-NOW] Largo alcance (LR 250k): %s\n", r == ESP_OK ? "OK" : esp_err_to_name(r));
  }
#endif
    colaOta = xQueueCreate(4, sizeof(PaqueteOta));
    xTaskCreate(tareaOta, "ota", 4096, NULL, 1, NULL);
    esp_now_register_send_cb(alEnviar);
    esp_now_register_recv_cb(alRecibir);
  } else {
    Serial.println("[ESP-NOW] No arranca; solo radio");
  }

  randomSeed(esp_random());
  Serial.printf("[ESP-NOW] MAC del nodo: %s\n", WiFi.macAddress().c_str());
#if MODO_REPETIDOR
  Serial.printf("%s -> REPETIDOR (id %d)\n", FW_VERSION, NODO_ID);
#else
  Serial.printf("%s -> Nodo %d\n", FW_VERSION, NODO_ID);
#endif
  Serial.printf("Arranque: %s%s\n", motivo, pendienteVerificar ? " (firmware nuevo por OTA, sin confirmar)" : "");
}

#if MODO_REPETIDOR
void loop() {
  static unsigned long tAviso = 0, tEstado = 0;
  alimentarVigilante();
  if (canalPuente != 0) firmwareProbado = true;
  comprobarFirmwareNuevo();
  if (canalPuente == 0) {
    if (!busquedaHecha || millis() - tUltimaBusqueda > BUSQUEDA_CADA_MS) {
      busquedaHecha = true;
      tUltimaBusqueda = millis();
      buscarPuente();
    } else if (canalUltimo != 0 && millis() - tAviso > 5000) {
      tAviso = millis();
      probarCanal(canalUltimo);
    }
  } else {
    atenderRepetidor();
    enviarInfo();
    if (millis() - tAviso >= 1000) { tAviso = millis(); emitirAviso(); }
  }
  if (millis() - tEstado >= 60000) {
    tEstado = millis();
    Serial.printf("[REP] puente=%s reenviados=%lu fallidos=%lu ota=%lu\n",
                  canalPuente ? "SI" : "no", reenviados, reenviosFallidos, otaReenviados);
  }
  delay(2);
}
#else
void loop() {
  // Si no hay puente: busqueda completa al arrancar y cada minuto; entre medias,
  // en cada lectura se escucha el ultimo canal bueno para recuperarse enseguida.
  alimentarVigilante();
  if (canalPuente == 0) {
    if (!busquedaHecha || millis() - tUltimaBusqueda > BUSQUEDA_CADA_MS) {
      busquedaHecha = true;
      tUltimaBusqueda = millis();
      buscarPuente();
    } else if (canalUltimo != 0) {
      probarCanal(canalUltimo);
    }
  }

  alimentarVigilante();
  comprobarFirmwareNuevo();
  enviarInfo();
  int valorBruto = leerSondaBruto();
  int porcentaje = brutoAPorcentaje(valorBruto);
  uint16_t codigo = (uint16_t)NODO_ID * 1000 + (uint16_t)porcentaje;

  if (enviarPorEspNow(porcentaje)) {
    fallosSeguidos = 0;
    firmwareProbado = true;
    Serial.printf(">>> Nodo %d -> %d%% (bruto=%d) por ESP-NOW\n", NODO_ID, porcentaje, valorBruto);
  } else {
    enviarPorRadio(codigo);
    Serial.printf(">>> Nodo %d -> %d%% (bruto=%d) por RADIO\n", NODO_ID, porcentaje, valorBruto);
    if (canalPuente != 0 && ++fallosSeguidos >= ESPNOW_FALLOS_PARA_BUSCAR) {
      Serial.println("[ESP-NOW] Varios fallos seguidos: buscare el puente de nuevo");
      canalPuente = 0;          // el puente puede haber cambiado de canal
      fallosSeguidos = 0;
      tUltimaBusqueda = millis();
    }
  }

  // Pausa con un poco de azar para que los nodos no coincidan siempre a la vez
  delay(PERIODO_LECTURA_MS + random(0, 1000));
}
#endif
