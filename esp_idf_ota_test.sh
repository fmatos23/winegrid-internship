#!/usr/bin/env bash
# Origem: escrito de raiz nesta colaboração (Claude), não copiado de nenhum
# repositório.
# Build, flash and test native OTA (ESP-IDF) on esp32_devkitc_wroom
# Usage: ./esp_idf_ota_test.sh [build|server|flash|bump|all]
# Default: all
#
#   build  - compile the app and stage the binary for the HTTP server
#   server - start the local HTTP server (if not already running) that
#            serves the built binary to the device over Wi-Fi
#   flash  - flash over USB + open the monitor (only needed once, or when
#            recovering the board - after that, updates arrive over OTA)
#   bump   - increment version.txt, rebuild, and re-stage for the server
#            (this is how you push a "new" OTA update without a cable)
#   all    - build + server + flash

set -e

IDF_EXPORT=~/esp/v5.5.1/esp-idf/export.sh
WINEGRID_DIR=~/Desktop/winegrid
PROJECT_DIR="$WINEGRID_DIR/esp-idf-ota"
SERVER_PORT=8070
SERVER_LOG="$WINEGRID_DIR/http_ota_server.log"
STEP=${1:-all}

source "$IDF_EXPORT" > /dev/null
cd "$PROJECT_DIR"

build() {
    echo "==> Building native_ota_example"
    idf.py build
    cp build/native_ota.bin build/native_ota_example.bin
    echo "==> Staged build/native_ota_example.bin for the HTTP server"
}

server() {
    if pgrep -f "http.server $SERVER_PORT" > /dev/null; then
        echo "==> HTTP server already running on port $SERVER_PORT"
    else
        echo "==> Starting HTTP server on port $SERVER_PORT"
        cd "$PROJECT_DIR/build"
        nohup python3 -m http.server "$SERVER_PORT" > "$SERVER_LOG" 2>&1 &
        disown
        cd "$PROJECT_DIR"
        sleep 1
    fi
    curl -s -o /dev/null -w "    Server check: HTTP %{http_code}\n" \
        "http://localhost:$SERVER_PORT/native_ota_example.bin"
}

flash() {
    echo "==> Flashing over USB (Serial Monitor in VSCode must be stopped)"
    idf.py -p /dev/ttyACM0 flash monitor
}

bump() {
    current=$(cat version.txt)

    if [[ "$current" == *.* ]]; then
        # "3.2" -> "3.3": increment the last dot-separated segment.
        prefix="${current%.*}"
        last="${current##*.}"
        if ! [[ "$last" =~ ^[0-9]+$ ]]; then
            echo "Erro: não consigo incrementar '$current' automaticamente (o último segmento, '$last', não é um número)."
            echo "Edita o version.txt à mão para o valor que quiseres, depois corre: $0 build"
            exit 1
        fi
        next="$prefix.$((last + 1))"
    elif [[ "$current" =~ ^[0-9]+$ ]]; then
        # "4" -> "5"
        next=$((current + 1))
    else
        echo "Erro: não consigo incrementar '$current' automaticamente (não é numérico nem tipo X.Y)."
        echo "Edita o version.txt à mão para o valor que quiseres, depois corre: $0 build"
        exit 1
    fi

    echo "==> Bumping version.txt: $current -> $next"
    echo "$next" > version.txt
    build
    server
    echo "==> New version $next staged. The device will pick it up on its next"
    echo "    periodic re-check (no manual reset needed, once it's running the"
    echo "    auto-recheck firmware)."
}

case "$STEP" in
    build) build ;;
    server) server ;;
    flash) flash ;;
    bump) bump ;;
    all)
        build
        server
        flash
        ;;
    *)
        echo "Uso: $0 [build|server|flash|bump|all]"
        exit 1
        ;;
esac
