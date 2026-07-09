# Notas de Estágio — Winegrid ESP32 Device-Management Platform

Registo cronológico de decisões, testes e descobertas. Usar como base para relatórios.

## 2026-07-07

**Contexto / orientação recebida (Joao Rodrigues)**
- Testar primeiro as plataformas/protocolos mais abrangentes de device management (ex: LwM2M), e só depois descer progressivamente para soluções menos completas (HTTP polling, MCUboot+mcumgr "cru").
- Manter este documento com bullet points de encontros, decisões e descobertas, para facilitar um relatório final.
- Três abordagens de OTA identificadas para avaliar:
  1. MCUboot + mcumgr (via BLE/serial)
  2. HTTP — sensor faz HTTP GET a um endpoint do backend
  3. LwM2M — protocolo de device management com update de firmware embutido

**Trabalho feito: PoC MCUboot + mcumgr via serial (ESP32 DevKitC WROOM)**
- Setup: MCUboot como 2º stage bootloader + app `smp_svr` (sample Zephyr) com mcumgr transport UART.
- App tornada self-contained em `winegrid/smp_svr/` (copiada do sample Zephyr, removido código BT não usado no transporte serial).
- Script `esp32_ota_test.sh` automatiza build+flash de MCUboot e da app, e testes via `ota_update.py` (upload de imagem nova, test-boot, echo de verificação, confirm).
- **OTA via serial funciona**: upload para slot secundário, test-boot, confirmação — ciclo completo validado (log mostra `Swap type: none` porque o modo é upgrade-only, ver achado abaixo).

**Achado importante: rollback automático não funciona out-of-the-box no ESP32**
- Configuração default da Espressif para MCUboot no ESP32 (`socs/esp32_procpu.conf`) define `CONFIG_BOOT_UPGRADE_ONLY=y` — não há swap com possibilidade de reversão, a imagem nova sobrescreve a antiga permanentemente.
- O mesmo ficheiro também desativa verificação de assinatura (`CONFIG_BOOT_SIGNATURE_TYPE_NONE=y`) e validação da slot0 — ou seja, **nem rollback nem secure boot estão ativos por omissão**.
- Tentativa de ativar `CONFIG_BOOT_SWAP_USING_SCRATCH=y` (algoritmo de swap clássico, a partição scratch já existe no layout) **causou crash loop no MCUboot** (`Fatal exception: IllegalInstruction`, watchdog reset em loop) — revertido para o default funcional.
- Hipótese: possivelmente relacionado com o mapeamento de flash via MMU/cache do ESP32 (janelas de endereço fixas por slot), diferente do XIP uniforme assumido pelo algoritmo de swap em Cortex-M. Não confirmado — precisaria de mais investigação se for necessário retomar este caminho (`swap-using-move`, ou usar o mecanismo OTA nativo do ESP-IDF em vez do MCUboot genérico do Zephyr).
- Confirmado por código-fonte (`img_mgmt_state.c` do Zephyr): em modo `upgrade-only`, o próprio firmware **rejeita estruturalmente** qualquer pedido de "marcar para teste" (`confirm=False`, erro `IMAGE_SETTING_TEST_TO_ACTIVE_DENIED`) — não é um bug nem estado corrompido (confirmado ao reproduzir o mesmo erro após `erase_flash` completo + reinstalação do zero). Este modo simplesmente não tem fase de teste: upload é sempre direto e permanente. `ota_update.py` foi ajustado para refletir isto (upload → confirm direto → reset, sem tentativa de test-boot).

**Resultado final validado**
- Fluxo completo `upload → confirm direto → reset → app viva na imagem nova` funciona de forma fiável e repetível (upgrade-only mode, sem fase de teste).
- Confirmado via código-fonte + reprodução consistente (inclusive após `erase_flash` completo) que a rejeição do test-boot é comportamento estrutural do modo `upgrade-only`, não bug de estado.
- Nota lateral: `ccache` está a cachear builds ignorando `__TIME__`, por isso builds sucessivos do `smp_svr` produzem hash idêntico — não invalida os testes, mas não dá para usar o hash/timestamp para distinguir versões visualmente sem limpar a cache.

**Decisão**
- PoC de MCUboot+mcumgr/serial dado como concluído — OTA básico validado, rollback identificado como limitação sem mudanças mais profundas ao MCUboot.
- Dado o conselho do orientador e o facto de o teste serial não refletir o caso de uso real (que precisa de ser sem fios), pausar a via MCUboot+mcumgr/serial aqui.
- Próximo passo: avaliar LwM2M como candidato mais abrangente antes de voltar a HTTP ou aprofundar mcumgr via Wi-Fi/UDP.

**Ficheiros relevantes**
- `esp32_ota_test.sh` — script de build/flash/test
- `ota_update.py` — script Python de OTA via mcumgr (upload + confirm direto, dado o modo upgrade-only; `--try-test-mode` demonstra a rejeição do test-boot)
- `smp_svr/` — app self-contained
- `mcuboot_swap.conf` — overlay testado para swap-with-scratch (não usado atualmente pelo script, mantido como registo)
- `criterios_avaliacao_plataformas.md` — tabela de critérios de avaliação

## 2026-07-08

**Trabalho iniciado: PoC LwM2M (ESP32 + Wi-Fi)**
- Criada `winegrid/lwm2m/` — sample `lwm2m_client` do Zephyr tornado self-contained (mesmo padrão do `smp_svr/`): `lwm2m-client.c` (registo de Objects + arranque do cliente), `firmware_update.c` (Object 5, ainda placeholder — não liga a MCUboot), `led.c`/`temperature.c`/`timer.c` (Objects IPSO de demo), `Kconfig` (define `LWM2M_APP_SERVER`, `LWM2M_APP_ID`).
- Adicionada config de Wi-Fi (`overlay-wifi.conf` + `esp32_wifi.overlay`, mergeados do sample `zephyr/samples/net/wifi`): liga-se à rede via shell manual (`wifi connect "<SSID>" 0 "<pass>"`) porque esta versão do Zephyr (3.7.0) não tem subsistema de credenciais Wi-Fi persistentes — não dá para hardcode-ar SSID/password no build.
- `prj.conf` configurado com `LWM2M_APP_SERVER=coap://192.168.82.56:5683` (IP do portátil na Wi-Fi — **muda se o IP mudar**) e `LWM2M_APP_ID=esp32-winegrid`.
- Criado `lwm2m_test.sh` (build/flash) — ainda não testado (build ainda não corrido).

**Servidor Leshan (para testar o cliente)**
- `wget` direto do CI da Eclipse deu 403 (bloqueio de bot/user-agent).
- Releases do GitHub (`eclipse/leshan`) **não têm `.jar` pré-compilado anexado** em nenhuma versão (nem a M18 mais recente nem a 1.5.0) — só código-fonte. Confirmado via API do GitHub (`assets: []`).
- M18 (mais recente) exige Java 17 (migrou para Jetty 12/Jakarta 10); só temos Java 11 instalado.
- Solução: clonado `eclipse/leshan` na tag `leshan-1.5.0` (compatível com Java 11) para `winegrid/leshan-src/`, compilado com `mvn install -DskipTests`. Jar funcional gerado em `leshan-server-demo/target/leshan-server-demo-1.5.0-jar-with-dependencies.jar`, copiado para `winegrid/leshan-server-demo.jar`.
- Servidor testado e a correr: `coap://0.0.0.0:5683` (LwM2M) + `http://0.0.0.0:8080` (UI web), log em `winegrid/leshan.log`.

**Resultado: primeiro registo LwM2M bem-sucedido**
- Build funcionou à primeira tentativa (sem erros de Kconfig/devicetree — `led.c`/`temperature.c`/`timer.c` compilaram bem para a `esp32_devkitc_wroom`). Uso de RAM mais alto que o `smp_svr` (68% `dram0_0_seg`), esperado dada a stack de rede + mbedTLS.
- **Bug de config encontrado e corrigido**: `CONFIG_NET_CONFIG_NEED_IPV4=n`/`NEED_IPV6=n` sozinhos não bastam para desligar os endereços estáticos de teste (`192.0.2.1`) herdados do `prj.conf` base — é preciso desligar `CONFIG_NET_CONFIG_SETTINGS=n` por inteiro. Corrigido em `overlay-wifi.conf`.
- **Sintaxe correta do shell Wi-Fi** (diferente do que estava documentado no README do sample): `wifi connect -s "<SSID>" -p "<password>" -k 1` (flags, não posicional).
- **Achado de comportamento**: o cliente LwM2M começa a tentar registar-se imediatamente no boot, sem esperar pela ligação Wi-Fi manual — esgota as tentativas (6 retries, ~32s) e para (`state 0`) antes de teres tempo de ligar o Wi-Fi à mão. Para reiniciar sem reboot: `lwm2m stop` seguido de `lwm2m start esp32-winegrid` no shell.
- **Achado importante — Wi-Fi não é persistente**: qualquer reset da placa (incluindo o reset físico que o Serial Monitor do VSCode dispara ao abrir/fechar) perde a associação Wi-Fi por completo (sem subsistema de credenciais nesta versão do Zephyr). Isto é uma limitação a registar na avaliação — não é viável para um dispositivo "no campo" sem mais trabalho (ex: guardar credenciais em NVS e reconectar automaticamente no boot).
- **Confirmado**: `esp32-winegrid` registado com sucesso no Leshan (`http://localhost:8080`) — primeiro marco importante do PoC LwM2M alcançado. Rede real (Wi-Fi) + protocolo de registo a funcionar de ponta a ponta.

**Problema resolvido: Wi-Fi auto-connect no arranque**
- Solução implementada (sem esperar por versão mais recente do Zephyr com subsistema de credenciais): `main()` em `lwm2m-client.c` agora chama `net_mgmt(NET_REQUEST_WIFI_CONNECT, ...)` diretamente no arranque — a mesma API que o comando `wifi connect` do shell usa por trás, só que automática. Credenciais (`CONFIG_APP_WIFI_SSID`/`APP_WIFI_PSK`) definidas em `overlay-wifi.conf`.
- Isto também corrige de raiz o bug de timing anterior: como agora há sempre um pedido de ligação real emitido, o `main()` espera corretamente pelo evento `L4_CONNECTED` antes de arrancar o cliente LwM2M — antes, sem Wi-Fi automático, essa espera era saltada (`conn_mgr_if_connect()` falhava logo) e o registo LwM2M começava a tentar-se sem rede nenhuma.
- **Trade-off registado**: as credenciais ficam guardadas em texto simples no binário compilado — aceitável para PoC numa rede de laboratório, mas **não é seguro para produção** (qualquer pessoa com acesso ao binário/flash consegue extrair a password). Para produção seria preciso usar armazenamento seguro (ex: NVS encriptado, ou o subsistema `wifi_credentials` disponível em versões mais recentes do Zephyr) — a registar como limitação/trabalho futuro na tabela de avaliação.
- Build validado, mas primeiro teste ainda falhou com o mesmo erro (`Failed to send packet, err 22`) — o registo continuava a arrancar cedo demais.
- **Segunda causa raiz encontrada**: a interface recebe um endereço **IPv6 link-local (SLAAC)** assim que fica ativa, sem precisar de DHCP nem de rede real nenhuma. O Connection Manager do Zephyr conta isso como "L4 ready" (`has_ip = has_ipv6 || has_ipv4`) e dispara `NET_EVENT_L4_CONNECTED` antes do IPv4 (DHCP) real estar pronto. Corrigido com `CONFIG_NET_IPV6=n` em `overlay-wifi.conf`, já que só precisamos de IPv4 para falar com o Leshan.
- Build validado sem erros com esta segunda correção, mas teste **continuou a falhar da mesma forma** (`err 22` logo no arranque, sem esperar rede nenhuma) — sinal de que a causa raiz ainda não tinha sido encontrada.
- **Causa raiz real, finalmente encontrada** (depois de inspecionar o binário compilado com `nm`/`strings`): `CONFIG_NET_CONNECTION_MANAGER` **nunca esteve ativo** em nenhuma das configurações anteriores. É um `menuconfig` do Zephyr, não é selecionado automaticamente por mais nada que tivéssemos ligado. O `main()` usa `if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) { ...espera pela rede... }` — com a opção desligada, o compilador **eliminava esse bloco inteiro como código morto**, incluindo toda a lógica original de espera pela rede E o nosso `wifi_auto_connect()` novo. Confirmado de forma inequívoca: a string `"Watgrid Guest"` simplesmente não existia no binário compilado (verificado com `strings`/`nm` no `.elf`), apesar do código-fonte e do Kconfig estarem corretos.
- Isto explica **todos** os problemas de timing que fomos vendo ao longo do dia (registo a começar demasiado cedo, antes mesmo da correção do IPv6) — não era só o SLAAC, era a espera pela rede a não existir de todo, desde o início.
- Corrigido com `CONFIG_NET_CONNECTION_MANAGER=y` explícito em `overlay-wifi.conf`. Rebuild confirma agora a string `"Watgrid Guest"` e `"Auto-connecting to Wi-Fi SSID"` presentes no binário — o código está finalmente a ser compilado e alcançável.
- **Lição a reter**: quando um `if (IS_ENABLED(CONFIG_X))` parece não ter efeito nenhum mesmo com o código aparentemente correto, vale a pena confirmar se `CONFIG_X` está mesmo definido em `autoconf.h`/`.config` — o compilador elimina esses blocos silenciosamente sem aviso nenhum quando a opção está desligada.

**Confirmado: auto-connect a funcionar de ponta a ponta**
- Flash + boot sem qualquer comando manual → `esp32-winegrid` aparece registado no Leshan sozinho. Resolve por completo a limitação de persistência de Wi-Fi identificada mais cedo hoje.
- Nota: o Registration ID muda a cada novo boot (comportamento normal do protocolo — cada arranque faz um registo novo, não um "update" de um registo existente, logo o servidor atribui sempre um ID de sessão novo).

**OTA real via Object 5 (Firmware Update) — implementado**
- `firmware_update.c` reescrito: `firmware_block_received_cb()` agora escreve os bytes recebidos diretamente na slot secundária usando `flash_img_init()`/`flash_img_buffered_write()` (`zephyr/dfu/flash_img.h`) — a mesma camada de baixo nível que o `mcumgr` usa por baixo, mas aqui alimentada pelos blocos CoAP do LwM2M em vez do protocolo mcumgr.
- `firmware_update_cb()` (disparado pelo recurso "Update" 5/0/2) chama `boot_request_upgrade(BOOT_UPGRADE_PERMANENT)` e depois `sys_reboot()` — permanente porque já sabemos (do PoC mcumgr) que este MCUboot é `upgrade-only`, sem fase de teste.
- **Bug encontrado no sample original**: `lwm2m_firmware_set_update_cb()` só era chamado dentro do `if (IS_ENABLED(CONFIG_LWM2M_FIRMWARE_UPDATE_PULL_SUPPORT))` — mas o trigger do recurso "Update" passa sempre por este callback, mesmo em modo push (upload direto, sem PULL). Sem esta correção, o botão de "Update" no Leshan não faria nada. Movido para fora do bloco condicional.
- Novo overlay `overlay-ota.conf`: ativa `CONFIG_BOOTLOADER_MCUBOOT=y` + `IMG_MANAGER`/`STREAM_FLASH`/`FLASH_MAP`/`REBOOT`, para a app gerar uma imagem assinada compatível com o MCUboot já instalado (partilhado com o `smp_svr`, não precisa de reflash).
- Build validado com sucesso — `mcuboot_hdr`/`metadata` a 100% confirmam assinatura correta; `zephyr.signed.bin` gerado.
- **Primeira tentativa de upload real via Leshan falhou**: `500 Request cancelled` no browser, e **nenhum** log `FIRMWARE: BLOCK RECEIVED` apareceu no dispositivo durante os ~6 minutos de tentativa — o upload nunca chegou de facto ao nosso código.
- **Causa raiz**: esgotamento de buffers de rede no dispositivo (`net_pkt: Data buffer allocation failed`, `esp32_wifi: Failed to allocate net buffer`). Os valores `NET_PKT_RX_COUNT`/`NET_BUF_RX_COUNT` (10 cada) herdados do `prj.conf` base estavam dimensionados para o teste em QEMU/native_sim do sample original, não para uma transferência real de ~680KB em ~1330 blocos CoAP pela Wi-Fi, com tráfego periódico de registo LwM2M a acontecer ao mesmo tempo.
- Corrigido: `NET_PKT_RX_COUNT`/`TX_COUNT`/`NET_BUF_RX_COUNT`/`TX_COUNT` subidos de 10 para 30 em `overlay-wifi.conf`. Build validado (RAM sobe ligeiramente para ~70%, ainda dentro do limite).
- **Por testar**: reflash + repetir o upload via Leshan.

- **Segunda tentativa (buffers de rede maiores) chegou mais longe, mas crashou**: os buffers resolveram a receção — viram-se finalmente logs `FIRMWARE: BLOCK RECEIVED` reais. Mas a placa crashou a meio do 2º bloco CoAP com `FATAL EXCEPTION ... illegal instruction` na thread `lwm2m-sock-recv`, e o Leshan reportou "no more registered" (dispositivo caiu da rede).
- **Causa raiz**: stack overflow. `firmware_block_received_cb()` chama `flash_img_buffered_write()` diretamente a partir da thread de receção de sockets do LwM2M (`lwm2m-sock-recv`), cujo stack default (`CONFIG_LWM2M_ENGINE_STACK_SIZE=2560`) não tem margem para a cadeia de chamadas de uma escrita real em flash (stream_flash → driver → helpers do MCUboot bootutil) por cima do que já estava a ser usado a fazer parsing de CoAP.
- Corrigido: `CONFIG_LWM2M_ENGINE_STACK_SIZE=8192` em `overlay-ota.conf`. Build validado (RAM sobe para ~72.5%, ainda com margem).
- **Terceira tentativa (stack maior)**: chegou mais longe (sobreviveu à 1ª escrita real de flash, offset 512), mas crashou/travou silenciosamente na **2ª** escrita real (offset 1024) — sem sequer imprimir o `FATAL EXCEPTION` desta vez.
- **Achado mais profundo (confiança moderada, não 100% confirmado)**: o `flash_img` só escreve mesmo na flash a cada `CONFIG_IMG_BLOCK_BUF_SIZE=512` bytes acumulados — e ambos os crashes coincidem exatamente com esses momentos de escrita real. Hipótese: no ESP32, escrever/apagar a flash SPI suspende temporariamente o acesso à cache de instruções (usada também pelo driver Wi-Fi, ativo durante a transferência). Se uma interrupção Wi-Fi disparar nessa janela crítica e o seu código não estiver todo em IRAM, o CPU tenta buscar instruções de uma zona de flash suspensa → crash ("illegal instruction"). Consistente com o `smp_svr` (mcumgr por série, sem Wi-Fi) nunca ter tido este problema.
- **Mitigação tentada**: `firmware_block_received_cb()` agora envolve a chamada `flash_img_buffered_write()` com `irq_lock()`/`irq_unlock()`, para impedir que a interrupção do Wi-Fi dispare durante a escrita. É uma correção "bruta" (esfomeia todas as interrupções durante a escrita), mas serve para testar/confirmar a hipótese. Build validado.
- **Quarta tentativa (com `irq_lock()`) — não crashou, mas revelou um efeito secundário**: em vez de crash, o download reiniciou do zero duas vezes seguidas (`offset:0` repetido) e depois parou, sem nunca ultrapassar o 1º bloco de ~512 bytes. Bloquear interrupções durante cada escrita atrasa a resposta de rede (ACK ao Leshan) o suficiente para o CoAP considerar o pedido perdido e retransmitir — e como o nosso código reinicia (`flash_img_init()`) sempre que vê `offset==0`, cada retransmissão apaga o progresso.

**Conclusão desta frente — limitação documentada, não resolvida**
- O mecanismo de OTA via LwM2M (Object 5) está **funcionalmente implementado e parcialmente validado**: regista no servidor, recebe blocos reais via CoAP block-wise, e o código de escrita em flash (`flash_img` + `boot_request_upgrade`) está correto em termos de API/lógica — mas esbarra num **limite real da plataforma ESP32 + Zephyr + Wi-Fi**: escrever na flash SPI enquanto o Wi-Fi está ativo é uma operação de risco nesta combinação (suspende a cache de instruções que o driver Wi-Fi também precisa), e as mitigações tentadas (stack maior, `irq_lock`) ou não resolveram (crash persistente, só mais tarde) ou trocaram o crash por um travamento/reinício em loop.
- **Isto é, em si, um achado valioso para a avaliação**: mostra que "OTA via LwM2M sobre Wi-Fi" tem um risco de fiabilidade real nesta plataforma que não existe na via mcumgr/série (onde não há Wi-Fi concorrente com a escrita). Possíveis caminhos futuros (não explorados por falta de tempo): mover a escrita de flash para uma thread dedicada de baixa prioridade fora do caminho crítico de rede, usar buffering maior em RAM para reduzir o nº de operações de escrita reais, ou investigar se existe uma opção de coexistência flash/Wi-Fi mais madura no HAL da Espressif not exposta ainda pelo Zephyr.
- Código (`firmware_update.c`, overlays) fica tal como está — funcional para demonstrar o fluxo completo do protocolo (registo, escrita de blocos, chamada de update), mas **não fiável para uma atualização completa de ~680KB nesta configuração**.

**Próximos passos**
1. Consolidar/rever este documento para relatório
2. Considerar DTLS (`overlay-dtls.conf`, já copiado mas não testado) para avaliar o critério de segurança, se ainda houver tempo
3. Se a via LwM2M+Wi-Fi+OTA voltar a ser prioritária no futuro, atacar primeiro a fiabilidade da escrita de flash concorrente com Wi-Fi antes de mais nada

**Ficheiros relevantes (LwM2M)**
- `lwm2m/` — app self-contained
- `lwm2m_test.sh` — script de build/flash
- `leshan-src/` — clone do Leshan (tag 1.5.0) usado para compilar o servidor
- `leshan-server-demo.jar` — servidor compilado, pronto a correr
- `leshan.log` — log do servidor a correr

## 2026-07-09

**Diagnóstico pedido pelo orientador (João) — antes de mais investigação**
- Conselho: antes de assumir que o crash de ontem é uma limitação da plataforma, eliminar hipóteses de ambiente local (rede Wi-Fi específica incompatível — WPA3? MTU alto? clock errado?) e de hardware específico desta placa.
- Verificado via `nmcli` (sem precisar de trocar de rede): a rede "Watgrid Guest" usa **WPA2** (não WPA3), confirmado no router — e o nosso código já forçava explicitamente WPA2-PSK de qualquer forma. MTU confirmado em 1500 (valor standard), sem anomalia. Clock SPI da flash em 40MHz (valor standard, sem erros reportados).
- Argumento adicional: o crash acontecia sempre **exatamente no mesmo ponto lógico** (a cada 512 bytes, o limite do buffer interno do `flash_img`) em todas as tentativas — repetibilidade perfeita, típica de bug de software/condição de corrida, não de instabilidade de rede (que tende a ser mais aleatória).

**Início da avaliação da "Plataforma 2": ESP-IDF nativo**
- ESP-IDF v5.5.1 já estava instalado localmente (`~/esp/v5.5.1/esp-idf`), de setembro 2025 — não foi preciso reinstalar nada. (Nota: `~/esp/esp-idf`, sem sufixo de versão, está numa branch de desenvolvimento `master`/6.0-dev — não usar essa, usar sempre a `v5.5.1`.)
- Corrigido um problema de ambiente: `setuptools` do sistema tinha sido atualizado para uma versão (80.9.0) incompatível com o requisito do ESP-IDF (`<71.0.1`) — resolvido com `pip install "setuptools<71.0.1,>=21"` no venv correto (`idf6.0_py3.10_env`, que apesar do nome tinha sido gerado para a 5.5).
- Testado `native_ota_example` (usa a API nativa `app_update`, não o `esp_https_ota`): compila, faz flash e arranca à primeira tentativa, sem nenhuma das dificuldades de devicetree/Kconfig que tivemos com o Zephyr.
- Esquema de partições mais robusto que o MCUboot: **3 partições de app** (`factory`, `ota_0`, `ota_1`) em vez de 2 slots, mais uma partição `otadata` dedicada a registar qual delas arrancar a seguir. Ficheiro real: `~/esp/v5.5.1/esp-idf/components/partition_table/partitions_two_ota.csv`.
- Mecanismo de OTA é **pull** (o dispositivo vai buscar o ficheiro a um URL HTTP/HTTPS), ao contrário do LwM2M que testámos ontem (**push**, o servidor envia blocos para o dispositivo).
- Tem controlo de versão nativo (compara versão do firmware novo com a que está a correr, recusa-se a repetir a mesma — proteção contra loops infinitos), e suporte a rollback documentado (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, com janela de "Diagnostics" no primeiro arranque) — **exatamente a funcionalidade que faltava no MCUboot+Zephyr**.

**RESULTADO: OTA completo e bem-sucedido, sem crash, à primeira tentativa**
- Setup: servidor HTTP simples (`python3 -m http.server 8070`) no portátil, a servir o `.bin` compilado; ESP32 configurado para ir buscar a `http://192.168.82.56:8070/native_ota_example.bin`.
- 1ª tentativa: dispositivo ligou-se, comparou versões (ambas =1), recusou-se corretamente a atualizar (comportamento esperado, não um erro).
- Subiu a versão para 2 no `version.txt`, recompilado (sem reflash — só disponibilizado no servidor). Dispositivo, já em loop de retry, apanhou a versão nova sozinho.
- **Resultado: os ~915KB do binário foram escritos por completo, sem nenhum crash, com o Wi-Fi ativo o tempo todo** — exatamente o cenário que crashava consistentemente no LwM2M/Zephyr. Reset limpo (`SW_CPU_RESET`, não um crash), reboot para a partição `ota_0`, `App version: 2` confirmado, hash SHA-256 do firmware diferente (prova de que é mesmo a imagem nova a correr).
- **Isto confirma, com bastante confiança, que o problema de ontem não era da placa nem da rede — era específico da implementação/coordenação flash+Wi-Fi no port do Zephyr para ESP32.** O ESP-IDF (SDK nativo da Espressif) já trata bem esta coordenação.

**Próximos passos**
1. Testar o mecanismo de rollback nativo (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) — a funcionalidade que faltava no Zephyr
2. Atualizar a tabela de avaliação e o relatório com este resultado (muda a conclusão geral: a limitação de OTA-por-Wi-Fi não é do ESP32 em si, é específica do Zephyr)
3. Continuar a avaliação do ESP-IDF nos restantes critérios (Secure Boot, provisioning, etc.)

**Ficheiros relevantes (ESP-IDF)**
- `esp-idf-ota/` — cópia self-contained do `native_ota_example`
- `http_ota_server.log` — log do servidor HTTP de teste
