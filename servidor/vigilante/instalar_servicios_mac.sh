#!/bin/bash
# =====================================================================
#  Instala en el Mac la web nueva y el vigilante como servicios.
#  Uso: bash ~/.riego/vigilante/instalar_servicios_mac.sh
#  No toca Mosquitto ni el gestor antiguo (solo los vigila).
# =====================================================================
set -u
DIR="$HOME/.riego/vigilante"
LA="$HOME/Library/LaunchAgents"
PY="$HOME/riego-inteligente/gestor/backend/venv/bin/python"
mkdir -p "$HOME/.riego/logs" "$LA"

if [ ! -x "$PY" ]; then echo "ERROR: no encuentro $PY"; exit 1; fi

echo "1) Paro la web arrancada a mano (si la hay)"
pkill -f 'web\.py$' 2>/dev/null && sleep 2 || true

for n in web vigilante; do
  PL="$LA/com.riego.$n.plist"
  echo "2) Servicio com.riego.$n"
  launchctl unload "$PL" 2>/dev/null || true
  cp "$DIR/com.riego.$n.plist" "$PL"
  launchctl load "$PL"
done

echo "3) Espero 15 s a que arranquen..."
sleep 15

echo "--- Servicios del riego ---"
launchctl list | awk 'NR==1 || /riego|jarvismac/'
echo "--- Web 8081 ---"
CODIGO=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8081/login)
[ "$CODIGO" = "200" ] && echo "OK (200)" || echo "FALLA (codigo $CODIGO) -> mira ~/.riego/logs/web.log"
echo "--- Vigilante (ultimas lineas) ---"
tail -5 "$HOME/.riego/logs/vigilante.log" 2>/dev/null
echo "--- Aviso de prueba ---"
"$PY" "$DIR/vigilante.py" --prueba
