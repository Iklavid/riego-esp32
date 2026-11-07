#!/bin/bash
# =====================================================================
#  Compila e instala Mosquitto 2.0.23 en el Mac sin Homebrew
#  (Homebrew falla en macOS Monterey al generar la documentacion).
#  Se instala en ~/.riego/mosquitto-bin (no necesita sudo).
#  Registro: ~/compilar-mosquitto.log
# =====================================================================
set -e
export PATH=/usr/local/bin:/usr/local/sbin:$HOME/Library/Python/3.9/bin:$HOME/.local/bin:$PATH

VERSION=2.0.23
PREFIJO="$HOME/.riego/mosquitto-bin"
TRABAJO="$HOME/.riego/compilacion"

echo "== Comprobando herramientas"
if ! command -v cmake >/dev/null; then
  echo "cmake no esta: lo instalo con pip (solo para tu usuario)"
  python3 -m pip install --user --quiet cmake
  export PATH="$(python3 -m site --user-base)/bin:$PATH"
fi
cmake --version | head -1

OPENSSL=""
# Mosquitto 2.0 no es compatible con OpenSSL 4: se usa la serie 3
for d in /usr/local/opt/openssl@3.6 /usr/local/opt/openssl@3 /opt/homebrew/opt/openssl@3; do
  [ -f "$d/include/openssl/ssl.h" ] && OPENSSL="$d" && break
done
if [ -z "$OPENSSL" ]; then
  echo "ERROR: no encuentro OpenSSL. Hace falta para las contrasenas." ; exit 1
fi
echo "OpenSSL: $OPENSSL ($(grep -m1 OPENSSL_VERSION_TEXT "$OPENSSL/include/openssl/opensslv.h" | cut -d\" -f2))"

echo "== Descargando Mosquitto $VERSION"
mkdir -p "$TRABAJO" && cd "$TRABAJO"
rm -rf "mosquitto-$VERSION"
curl -fsSL -o "mosquitto-$VERSION.tar.gz" "https://mosquitto.org/files/source/mosquitto-$VERSION.tar.gz"
tar xzf "mosquitto-$VERSION.tar.gz"
cd "mosquitto-$VERSION" && mkdir -p build && cd build

echo "== Compilando (unos minutos)"
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIJO" \
  -DOPENSSL_ROOT_DIR="$OPENSSL" \
  -DCMAKE_C_FLAGS="-I$OPENSSL/include" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$OPENSSL/lib" \
  -DWITH_TLS=ON -DWITH_DOCS=OFF -DWITH_CJSON=OFF -DWITH_WEBSOCKETS=OFF \
  -DWITH_SRV=OFF -DWITH_STATIC_LIBRARIES=OFF -DDOCUMENTATION=OFF > ../cmake.log
make -j4 > ../make.log 2>&1 || { tail -30 ../make.log; exit 1; }
make install > ../install.log 2>&1

echo "== Instalado"
ls "$PREFIJO/sbin/mosquitto" "$PREFIJO/bin/mosquitto_passwd"
"$PREFIJO/sbin/mosquitto" -h 2>&1 | head -1
echo "INSTALADO"
