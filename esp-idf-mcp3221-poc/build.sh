#!/usr/bin/env bash
# Origem: escrito de raiz nesta colaboração (Claude), não copiado de
# nenhum repositório.
# Compila (só build - sem sensor físico não há o que testar em runtime) a
# prova de conceito do componente MCP3221 em ESP-IDF, equivalente à do
# Zephyr em zephyr-mcp3221-poc/.
# Usage: ./build.sh

set -e

IDF_EXPORT=~/esp/v5.5.1/esp-idf/export.sh
PROJECT_DIR=~/Desktop/winegrid/esp-idf-mcp3221-poc

source "$IDF_EXPORT" > /dev/null
cd "$PROJECT_DIR"
idf.py build