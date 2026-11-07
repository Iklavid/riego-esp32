# -*- coding: utf-8 -*-
"""
Cerebro del riego inteligente (Fase 4)
======================================
No riega por su cuenta: cada dia calcula un AJUSTE en % para cada zona y se lo
manda a la S3 (riego/s3/ajuste/set). La S3 lo aplica a sus programas solo si el
interruptor "Riego inteligente" esta activado, y si el ajuste caduca (36 h sin
recibir otro) vuelve sola al 100 %.

Como decide cada zona:
  demanda  = ET0 (Open-Meteo) x Kc(cultivo, mes) x sombra
  neta     = demanda - lluvia util (caida + prevista, ponderada)
  ajuste   = neta / referencia x aprendizaje x factor propio
  + humedad del sensor de la zona (si esta muy humeda no riega; si esta seca, mas)
  + seguridad: lluvia prevista, helada, limites minimo y maximo.

Como aprende:
  - Por zona: cada manana mira la humedad real del sensor. Si tras lo regado sigue
    seca, sube su factor; si esta encharcada, lo baja (poco a poco, con limites).
  - Por fuente del tiempo (Open-Meteo, AEMET): compara la probabilidad de lluvia
    que dieron con lo que paso de verdad (sensor de lluvia) y les da mas o menos
    confianza.

Modo PRUEBA (por defecto): calcula y lo apunta todo, pero NO envia nada a la S3.
"""
import os
import json
import time
import sqlite3
import logging
import datetime
import threading
import urllib.request
import urllib.parse

log = logging.getLogger("cerebro")

# Kc mensual (enero..diciembre), valores orientativos FAO-56 adaptados
CULTIVOS = {
    "frutales":        {"nombre": "Frutales (nispero, manzano, frambueso, laurel)",
                        "kc": [0.55, 0.55, 0.60, 0.70, 0.80, 0.90, 0.95, 0.95, 0.85, 0.75, 0.60, 0.55]},
    "huerto_verano":   {"nombre": "Huerto de verano (tomate, calabacin, melon, sandia, pimiento)",
                        "kc": [0.50, 0.50, 0.60, 0.75, 0.95, 1.05, 1.05, 1.00, 0.90, 0.75, 0.60, 0.50]},
    "huerto_invierno": {"nombre": "Huerto de invierno (col, coliflor, brocoli, cebolla, lechuga)",
                        "kc": [0.90, 0.95, 0.95, 0.90, 0.80, 0.70, 0.60, 0.60, 0.70, 0.80, 0.85, 0.90]},
    "huerto_mixto":    {"nombre": "Huerto mixto (cambio de temporada)",
                        "kc": [0.70, 0.72, 0.78, 0.82, 0.88, 0.88, 0.83, 0.80, 0.80, 0.78, 0.72, 0.70]},
    "rosas_vid":       {"nombre": "Rosas y vid",
                        "kc": [0.40, 0.40, 0.50, 0.60, 0.70, 0.75, 0.75, 0.75, 0.65, 0.55, 0.45, 0.40]},
    "papas_millo":     {"nombre": "Papas y millo",
                        "kc": [0.50, 0.50, 0.70, 0.90, 1.05, 1.10, 1.10, 1.00, 0.80, 0.60, 0.50, 0.50]},
    "cesped":          {"nombre": "Cesped",
                        "kc": [0.80, 0.80, 0.85, 0.90, 0.95, 0.95, 0.95, 0.95, 0.90, 0.85, 0.80, 0.80]},
    "sin_plantar":     {"nombre": "Sin plantar (no riega)", "kc": [0.0] * 12},
}

CONFIG_DEFECTO = {
    "modo": "prueba",                 # prueba = calcula sin enviar | activo = envia a la S3
    "hora_calculo": "05:00",
    "ubicacion": {"nombre": "", "lat": None, "lon": None, "altitud": None},
    "aemet": {"usar": False, "municipio": ""},   # código INE de 5 cifras (p. ej. 28079 = Madrid)
    "lluvia_saltar_mm": 4.0,          # si se preven >= mm ...
    "lluvia_saltar_prob": 70,         # ... con probabilidad >= %, no se riega
    "temp_min_helada": 3.0,           # si la minima prevista baja de esto, no se riega
    "zonas": {
        "2": {"activa": True, "cultivo": "frutales",     "sensor": 3, "hum_baja": 30, "hum_alta": 70,
              "sombra": 0, "referencia_mm": 5.0, "ajuste_min": 30, "ajuste_max": 150, "factor": 100},
        "3": {"activa": True, "cultivo": "huerto_mixto", "sensor": 2, "hum_baja": 35, "hum_alta": 75,
              "sombra": 0, "referencia_mm": 5.0, "ajuste_min": 40, "ajuste_max": 160, "factor": 100},
        "4": {"activa": True, "cultivo": "rosas_vid",    "sensor": 1, "hum_baja": 25, "hum_alta": 65,
              "sombra": 0, "referencia_mm": 5.0, "ajuste_min": 30, "ajuste_max": 150, "factor": 100},
        "5": {"activa": True, "cultivo": "sin_plantar",  "sensor": 4, "hum_baja": 30, "hum_alta": 70,
              "sombra": 0, "referencia_mm": 5.0, "ajuste_min": 30, "ajuste_max": 150, "factor": 100},
    },
}

APRENDIZAJE_DEFECTO = {
    "zonas": {},                      # "2": {"k": 1.0, "dias": 0}
    "fuentes": {"openmeteo": {"error": 0.25, "dias": 0}, "aemet": {"error": 0.25, "dias": 0}},
    "pendiente": None,                # previsiones de lluvia de ayer para comprobar hoy
}

K_MIN, K_MAX, K_PASO = 0.5, 2.0, 0.05
VALIDEZ_S = 36 * 3600


def _contexto_ssl():
    """El Python de python.org en macOS no trae los certificados raiz: uso los de certifi.
    (Nunca se desactiva la comprobacion del certificado.)"""
    import ssl
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()


SSL_CTX = _contexto_ssl()


def _get_json(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": "riego-inteligente/1.0"})
    with urllib.request.urlopen(req, timeout=timeout, context=SSL_CTX) as r:
        datos = r.read()
    try:
        return json.loads(datos.decode("utf-8"))
    except UnicodeDecodeError:
        return json.loads(datos.decode("latin-1"))


class Cerebro:
    def __init__(self, data_dir, ruta_clave_aemet, estado_fn, lluvia_dia_fn, publicar_fn, evento_fn):
        """estado_fn() -> {"nodos": {...}}   lluvia_dia_fn(fecha) -> True/False/None
           publicar_fn(topic, payload, retain)   evento_fn(origen, tipo, texto)"""
        self.dir = data_dir
        self.ruta_cfg = os.path.join(data_dir, "inteligente.json")
        self.ruta_apr = os.path.join(data_dir, "aprendizaje.json")
        self.ruta_db = os.path.join(data_dir, "historial.db")
        self.ruta_clave = ruta_clave_aemet
        self.estado_fn, self.lluvia_dia_fn = estado_fn, lluvia_dia_fn
        self.publicar_fn, self.evento_fn = publicar_fn, evento_fn
        self.lock = threading.Lock()
        self.ultimo = None
        os.makedirs(data_dir, exist_ok=True)
        with sqlite3.connect(self.ruta_db) as c:
            c.execute("""CREATE TABLE IF NOT EXISTS decisiones(
                id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, fecha TEXT, zona INTEGER,
                ajuste INTEGER, enviado INTEGER, detalle TEXT)""")
            c.execute("CREATE INDEX IF NOT EXISTS dec_fecha ON decisiones(fecha)")

    # ---------------------------------------------------------- ficheros
    def _leer(self, ruta, defecto):
        try:
            with open(ruta, encoding="utf-8") as f:
                d = json.load(f)
        except Exception:
            return json.loads(json.dumps(defecto))
        base = json.loads(json.dumps(defecto))
        base.update(d)
        return base

    def _guardar(self, ruta, d):
        tmp = ruta + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(d, f, ensure_ascii=False, indent=1)
        os.replace(tmp, ruta)

    def config(self):
        c = self._leer(self.ruta_cfg, CONFIG_DEFECTO)
        for z, d in CONFIG_DEFECTO["zonas"].items():          # zonas nuevas o campos nuevos
            c["zonas"].setdefault(z, dict(d))
            for k, v in d.items():
                c["zonas"][z].setdefault(k, v)
        return c

    def aprendizaje(self):
        return self._leer(self.ruta_apr, APRENDIZAJE_DEFECTO)

    def guardar_config(self, nueva):
        c = self.config()
        if nueva.get("modo") in ("prueba", "activo"):
            c["modo"] = nueva["modo"]
        h = str(nueva.get("hora_calculo", c["hora_calculo"]))
        if len(h) == 5 and h[2] == ":" and 0 <= int(h[:2]) <= 23 and 0 <= int(h[3:]) <= 59:
            c["hora_calculo"] = h
        u = nueva.get("ubicacion") or {}
        if u.get("lat") is not None and u.get("lon") is not None:
            lat, lon = float(u["lat"]), float(u["lon"])
            if not (-90 <= lat <= 90 and -180 <= lon <= 180):
                raise ValueError("Coordenadas no validas")
            c["ubicacion"] = {"nombre": str(u.get("nombre", ""))[:80], "lat": lat, "lon": lon,
                              "altitud": None if u.get("altitud") is None else float(u["altitud"])}
        a = nueva.get("aemet") or {}
        if "usar" in a:
            c["aemet"]["usar"] = bool(a["usar"])
        if str(a.get("municipio", "")).isdigit() and len(str(a["municipio"])) == 5:
            c["aemet"]["municipio"] = str(a["municipio"])
        for k, lo, hi in (("lluvia_saltar_mm", 0, 100), ("lluvia_saltar_prob", 0, 100), ("temp_min_helada", -10, 15)):
            if k in nueva:
                c[k] = max(lo, min(hi, float(nueva[k])))
        for z, d in (nueva.get("zonas") or {}).items():
            if z not in c["zonas"]:
                continue
            zc = c["zonas"][z]
            if "activa" in d:
                zc["activa"] = bool(d["activa"])
            if d.get("cultivo") in CULTIVOS:
                zc["cultivo"] = d["cultivo"]
            if "sensor" in d:
                s = int(d["sensor"] or 0)
                zc["sensor"] = s if 0 <= s <= 8 else 0
            for k, lo, hi in (("hum_baja", 0, 100), ("hum_alta", 0, 100), ("sombra", 0, 90),
                              ("referencia_mm", 1, 15), ("ajuste_min", 0, 100), ("ajuste_max", 100, 300),
                              ("factor", 20, 300)):
                if k in d:
                    zc[k] = max(lo, min(hi, float(d[k])))
            if zc["hum_baja"] >= zc["hum_alta"]:
                raise ValueError(f"Zona {z}: la humedad baja debe ser menor que la alta")
        self._guardar(self.ruta_cfg, c)
        return c

    def clave_aemet(self):
        try:
            with open(self.ruta_clave, encoding="utf-8") as f:
                return f.read().strip() or None
        except OSError:
            return None

    def guardar_clave_aemet(self, clave):
        clave = (clave or "").strip()
        os.makedirs(os.path.dirname(self.ruta_clave), mode=0o700, exist_ok=True)
        if not clave:
            if os.path.exists(self.ruta_clave):
                os.remove(self.ruta_clave)
            return
        if len(clave) < 20 or len(clave) > 2000:
            raise ValueError("La clave de AEMET no parece valida")
        tmp = self.ruta_clave + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(clave)
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.ruta_clave)

    # ---------------------------------------------------------- tiempo
    @staticmethod
    def buscar_lugar(texto):
        q = urllib.parse.urlencode({"name": texto, "count": 8, "language": "es", "format": "json"})
        d = _get_json("https://geocoding-api.open-meteo.com/v1/search?" + q)
        return [{"nombre": ", ".join(x for x in (r.get("name"), r.get("admin2") or r.get("admin3"),
                                                   r.get("admin1"), r.get("country")) if x),
                 "lat": r["latitude"], "lon": r["longitude"], "altitud": r.get("elevation")}
                for r in d.get("results", [])]

    @staticmethod
    def tiempo_openmeteo(lat, lon):
        q = urllib.parse.urlencode({
            "latitude": lat, "longitude": lon, "timezone": "auto", "past_days": 1, "forecast_days": 2,
            "daily": "et0_fao_evapotranspiration,precipitation_sum,precipitation_probability_max,"
                     "temperature_2m_min,temperature_2m_max"})
        d = _get_json("https://api.open-meteo.com/v1/forecast?" + q)["daily"]
        # indices: 0 = ayer, 1 = hoy, 2 = manana
        return {"fechas": d["time"], "et0": d["et0_fao_evapotranspiration"], "lluvia": d["precipitation_sum"],
                "prob": d["precipitation_probability_max"], "tmin": d["temperature_2m_min"],
                "tmax": d["temperature_2m_max"]}

    def prob_aemet(self, municipio):
        clave = self.clave_aemet()
        if not clave:
            return None
        url = ("https://opendata.aemet.es/opendata/api/prediccion/especifica/municipio/diaria/"
               f"{municipio}?api_key={urllib.parse.quote(clave)}")
        meta = _get_json(url)
        if meta.get("estado") != 200 or not meta.get("datos"):
            raise RuntimeError(f"AEMET: {meta.get('descripcion', 'sin datos')}")
        dias = _get_json(meta["datos"])[0]["prediccion"]["dia"]
        res = {}
        for d in dias[:2]:
            vals = [int(p["value"]) for p in d.get("probPrecipitacion", []) if str(p.get("value", "")).isdigit()]
            res[d["fecha"][:10]] = max(vals) if vals else None
        return res

    # ---------------------------------------------------------- aprendizaje
    def _aprender_fuentes(self, apr, hoy):
        """Compara lo que dijeron ayer las fuentes con lo que paso (sensor de lluvia)."""
        p = apr.get("pendiente")
        if not p or p.get("fecha") == hoy:
            return []
        llovio = self.lluvia_dia_fn(p["fecha"])
        notas = []
        if llovio is None:
            return notas
        for fuente, prob in (p.get("probs") or {}).items():
            if prob is None:
                continue
            f = apr["fuentes"].setdefault(fuente, {"error": 0.25, "dias": 0})
            err = abs(prob / 100.0 - (1.0 if llovio else 0.0))
            f["error"] = round(0.85 * f["error"] + 0.15 * err, 4)      # media movil
            f["dias"] += 1
            notas.append(f"{fuente}: dijo {prob}% y {'llovio' if llovio else 'no llovio'}")
        apr["pendiente"] = None
        return notas

    def _aprender_zona(self, apr, z, hum, zc):
        """Ajuste fino por zona segun la humedad real de esta manana."""
        a = apr["zonas"].setdefault(z, {"k": 1.0, "dias": 0})
        hoy = datetime.date.today().isoformat()
        if hum is None or a.get("ultima") == hoy:      # sin dato, o ya aprendio hoy
            return a["k"], None
        a["ultima"] = hoy
        nota = None
        if hum < zc["hum_baja"]:
            a["k"] = min(K_MAX, a["k"] * (1 + K_PASO)); nota = "seca tras lo regado: aprende a regar mas"
        elif hum > zc["hum_alta"]:
            a["k"] = max(K_MIN, a["k"] * (1 - K_PASO)); nota = "demasiado humeda: aprende a regar menos"
        a["k"] = round(a["k"], 3)
        a["dias"] += 1
        return a["k"], nota

    # ---------------------------------------------------------- calculo
    def calcular(self, enviar=None, aprender=True):
        with self.lock:
            return self._calcular(enviar, aprender)

    def _calcular(self, enviar, aprender):
        cfg = self.config()
        apr = self.aprendizaje()
        u = cfg["ubicacion"]
        if u["lat"] is None:
            raise ValueError("Falta la ubicacion: buscala en la pestana Inteligente")
        ahora = datetime.datetime.now()
        hoy = ahora.strftime("%Y-%m-%d")
        mes = ahora.month - 1

        tiempo = self.tiempo_openmeteo(u["lat"], u["lon"])
        fuentes_err = []
        probs = {"openmeteo": tiempo["prob"][1]}
        if cfg["aemet"]["usar"]:
            try:
                pa = self.prob_aemet(cfg["aemet"]["municipio"])
                probs["aemet"] = (pa or {}).get(hoy)
            except Exception as e:
                fuentes_err.append(f"AEMET no disponible: {e}")

        notas_fuentes = self._aprender_fuentes(apr, hoy) if aprender else []

        # Probabilidad de lluvia ponderada por la confianza aprendida de cada fuente
        num = den = 0.0
        for f, p in probs.items():
            if p is None:
                continue
            w = max(0.05, 1.0 - apr["fuentes"].get(f, {"error": 0.25})["error"])
            num += p * w; den += w
        prob_lluvia = round(num / den) if den else 0
        et0 = round(((tiempo["et0"][0] or 0) + (tiempo["et0"][1] or 0)) / 2, 2)
        lluvia_ayer = tiempo["lluvia"][0] or 0
        lluvia_hoy = tiempo["lluvia"][1] or 0
        tmin_hoy = tiempo["tmin"][1]
        lluvia_util = 0.8 * (lluvia_ayer if lluvia_ayer >= 2 else 0) + \
                      0.8 * (lluvia_hoy * prob_lluvia / 100.0 if lluvia_hoy >= 2 else 0)

        nodos = self.estado_fn().get("nodos", {})
        resultado = {}
        for z, zc in cfg["zonas"].items():
            d = {"cultivo": zc["cultivo"], "motivos": []}
            if not zc["activa"]:
                resultado[z] = {**d, "ajuste": 100, "motivos": ["cerebro desactivado en esta zona: programa normal"]}
                continue
            kc = CULTIVOS[zc["cultivo"]]["kc"][mes]
            if kc == 0:
                resultado[z] = {**d, "ajuste": 0, "motivos": ["zona sin plantar"]}
                continue
            hum = None
            n = nodos.get(str(zc["sensor"])) if zc["sensor"] else None
            if n and n.get("activo") and n.get("al_dia") and n.get("humedad") is not None:
                hum = n["humedad"]
            k, nota_k = self._aprender_zona(apr, z, hum, zc) if aprender else \
                (apr["zonas"].get(z, {}).get("k", 1.0), None)

            demanda = et0 * kc * (1 - zc["sombra"] / 100 * 0.5)
            neta = max(0.0, demanda - lluvia_util)
            ajuste = neta / zc["referencia_mm"] * 100 * k * zc["factor"] / 100
            d.update(et0=et0, kc=kc, demanda_mm=round(demanda, 2), lluvia_util_mm=round(lluvia_util, 2),
                     k_aprendido=k, humedad=hum, prob_lluvia=prob_lluvia)
            d["motivos"].append(f"ET0 {et0} mm x Kc {kc} = {demanda:.1f} mm; lluvia util {lluvia_util:.1f} mm")
            if nota_k:
                d["motivos"].append(nota_k)

            cero = None
            if tmin_hoy is not None and tmin_hoy < cfg["temp_min_helada"]:
                cero = f"riesgo de helada (minima {tmin_hoy} C)"
            elif lluvia_hoy >= cfg["lluvia_saltar_mm"] and prob_lluvia >= cfg["lluvia_saltar_prob"]:
                cero = f"se preven {lluvia_hoy} mm de lluvia con {prob_lluvia}% de probabilidad"
            elif hum is not None and hum >= zc["hum_alta"]:
                cero = f"tierra humeda ({hum}%)"
            if cero:
                ajuste = 0
                d["motivos"].append("NO RIEGA: " + cero)
            else:
                if hum is not None and hum <= zc["hum_baja"]:
                    ajuste *= 1.25
                    d["motivos"].append(f"tierra seca ({hum}%): +25%")
                elif hum is None and zc["sensor"]:
                    d["motivos"].append(f"sensor N{zc['sensor']} sin dato: solo tiempo")
                ajuste = max(zc["ajuste_min"], min(zc["ajuste_max"], ajuste))
            d["ajuste"] = int(round(ajuste / 5) * 5)
            resultado[z] = d

        if aprender:
            apr["pendiente"] = {"fecha": hoy, "probs": probs}
            self._guardar(self.ruta_apr, apr)

        envia = aprender and ((cfg["modo"] == "activo") if enviar is None else bool(enviar))
        enviado = False
        if envia:
            payload = {"ts": int(time.time()), "valido_hasta": int(time.time()) + VALIDEZ_S,
                       "zonas": {z: r["ajuste"] for z, r in resultado.items()}}
            try:
                self.publicar_fn("riego/s3/ajuste/set", json.dumps(payload, separators=(",", ":")), True)
                enviado = True
            except Exception as e:
                fuentes_err.append(f"No se pudo enviar a la S3: {e}")

        if not aprender:          # simulacion desde la web: no se apunta nada
            return {"ts": int(time.time()), "fecha": hoy, "zonas": resultado, "enviado": False, "simulacion": True,
                    "tiempo": tiempo, "probs": probs, "prob_lluvia": prob_lluvia, "avisos": fuentes_err}
        with sqlite3.connect(self.ruta_db) as c:
            for z, r in resultado.items():
                c.execute("INSERT INTO decisiones(ts,fecha,zona,ajuste,enviado,detalle) VALUES(?,?,?,?,?,?)",
                          (int(time.time()), hoy, int(z), r["ajuste"], int(enviado), json.dumps(r, ensure_ascii=False)))
        resumen = ", ".join(f"V{z} {r['ajuste']}%" for z, r in sorted(resultado.items()))
        self.evento_fn("cerebro", "cerebro",
                       f"Riego inteligente ({'enviado a la S3' if enviado else 'modo prueba, no enviado'}): {resumen}")
        self.ultimo = {"ts": int(time.time()), "fecha": hoy, "zonas": resultado, "enviado": enviado,
                       "tiempo": tiempo, "probs": probs, "prob_lluvia": prob_lluvia,
                       "aprendizaje_fuentes": notas_fuentes, "avisos": fuentes_err}
        return self.ultimo

    def decisiones(self, dias=30):
        desde = (datetime.date.today() - datetime.timedelta(days=dias)).isoformat()
        with sqlite3.connect(self.ruta_db) as c:
            filas = c.execute("SELECT ts,fecha,zona,ajuste,enviado,detalle FROM decisiones "
                              "WHERE fecha>=? ORDER BY id DESC LIMIT 2000", (desde,)).fetchall()
        return [{"ts": f[0], "fecha": f[1], "zona": f[2], "ajuste": f[3], "enviado": bool(f[4]),
                 "detalle": json.loads(f[5])} for f in filas]

    # ---------------------------------------------------------- bucle diario
    def bucle(self):
        hecho = None
        while True:
            try:
                cfg = self.config()
                ahora = datetime.datetime.now()
                hoy = ahora.strftime("%Y-%m-%d")
                if hecho != hoy and ahora.strftime("%H:%M") >= cfg["hora_calculo"] \
                        and cfg["ubicacion"]["lat"] is not None:
                    with sqlite3.connect(self.ruta_db) as c:
                        ya = c.execute("SELECT 1 FROM decisiones WHERE fecha=? LIMIT 1", (hoy,)).fetchone()
                    if not ya:
                        self.calcular()
                    hecho = hoy
            except Exception as e:
                log.error(f"Cerebro: {e}")
                self.evento_fn("cerebro", "cerebro", f"Riego inteligente: no se pudo calcular ({e})")
                time.sleep(1800)       # reintento en media hora
            time.sleep(60)
