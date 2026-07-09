# Critérios de Avaliação — Plataformas IoT para ESP32
Estágio Winegrid — ESP32 Device-Management Platform: Evaluation & PoC

## Tabela de Critérios (decisivos)

| Categoria | Critério | O que avaliar |
|---|---|---|
| **Gestão de Dispositivos** | OTA (updates remotos) | Suporte nativo? Mecanismo (A/B partitions, rollback)? |
| | Provisioning | Configuração inicial do dispositivo (Wi-Fi, credenciais) |
| **Segurança** | Secure Boot & armazenamento seguro | Suporte, maturidade, gestão de chaves/certificados |
| **Conectividade** | Wi-Fi + protocolos | Suporte nativo, MQTT/HTTP(S), TLS |
| **Esforço de Integração** | Tempo de setup | Horas até build funcional |
| | Nº de workarounds | Problemas encontrados vs. resolvidos |
| | Compatibilidade com caso de uso real | Cumpre o requisito sem "gambiarras"? |
| **Maturidade** | Documentação e suporte oficial Espressif | Qualidade de docs, nível de parceria/integração |

## Preenchimento por Plataforma

### Zephyr RTOS

| Critério | Avaliação |
|---|---|
| OTA | Duas vias testadas. **MCUboot+mcumgr/série**: funciona bem (upload→confirm→reset), mas placa só suporta modo `upgrade-only` (sem rollback automático — tentativa de ativar swap-with-scratch causou crash loop no bootloader). **LwM2M Object 5 (Firmware Update)/Wi-Fi**: protocolo implementado e parcialmente validado (regista, recebe blocos reais via CoAP), mas escrever a flash com Wi-Fi ativo é instável nesta plataforma — crash ("illegal instruction") ou travamento consistente a meio de updates grandes (~680KB), não resolvido apesar de 3 tentativas de mitigação. Nenhuma das duas vias tem rollback fiável testado no cenário sem fios. |
| Provisioning | Wi-Fi: sem subsistema de credenciais persistentes nesta versão do Zephyr (3.7.0) — só shell manual ou credenciais hardcoded no firmware (implementado como PoC, não seguro para produção). |
| Secure Boot & armazenamento seguro | Default da Espressif para MCUboot no ESP32 desativa verificação de assinatura (`BOOT_SIGNATURE_TYPE_NONE=y`). Não testado ativar — dado o crash ao mexer no modo de swap, é provável que precise de investigação semelhante. |
| Wi-Fi + protocolos | Wi-Fi nativo funciona bem (driver Zephyr para ESP32). LwM2M (CoAP/UDP) valida registo e comunicação sustentada com sucesso. DTLS não testado (por falta de tempo). |
| Tempo de setup | Um dia inteiro de trabalho intensivo, com bastante debugging de baixo nível (Kconfig, ccache, stack sizes, buffers de rede, interação flash/Wi-Fi). |
| Nº workarounds | Muitos: build dirs stale, conflitos de porta série, ccache a ignorar `__TIME__`, `CONFIG_NET_CONNECTION_MANAGER` desligado silenciosamente eliminava código morto, buffers de rede subdimensionados, stack overflow, crash de flash+Wi-Fi. |
| Compatibilidade com caso de uso | Parcial. O caso de uso real (sensor de campo, OTA sem fios) precisa exatamente da combinação que falhou (Wi-Fi + escrita de flash fiável). Via série funciona mas não reflete o cenário real (precisa de cabo). |
| Documentação / suporte Espressif | Samples oficiais existem e servem de bom ponto de partida, mas várias combinações (Wi-Fi + LwM2M + OTA) nunca foram testadas/validadas oficialmente juntas — exigiu bastante trabalho de integração e debugging não documentado. |

### ESP-IDF (nativo, v5.5.1)

| Critério | Avaliação |
|---|---|
| OTA | **Testado com sucesso à primeira tentativa, sem crash.** Mecanismo *pull* (dispositivo vai buscar o binário a um URL HTTP/HTTPS), via API nativa `app_update` (`native_ota_example`). ~915KB escritos por completo com Wi-Fi ativo o tempo todo — exatamente o cenário que crashava consistentemente no Zephyr/LwM2M. Esquema de 3 partições (`factory`, `ota_0`, `ota_1` + `otadata`), controlo de versão nativo (evita loops de update), e suporte documentado a rollback automático (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, com janela de diagnóstico no 1º arranque) — ainda por testar em detalhe, mas existe e está documentado, ao contrário do Zephyr. |
| Provisioning | Wi-Fi por credenciais no `sdkconfig` (mesmo trade-off do Zephyr — hardcoded, não seguro para produção tal como testado). Não explorado ainda um mecanismo de provisioning mais maduro (a Espressif tem `wifi_provisioning` component, por avaliar). |
| Secure Boot & armazenamento seguro | Não testado ainda. |
| Wi-Fi + protocolos | Wi-Fi nativo, ligação imediata e estável, sem nenhum dos bugs de timing encontrados no Zephyr (não foi preciso descobrir/ativar nenhum "Connection Manager" escondido). |
| Tempo de setup | Muito mais rápido que o Zephyr — ambiente já estava instalado; build+flash+boot+Wi-Fi+OTA completo funcionou em poucas horas, incluindo a pesquisa inicial. |
| Nº workarounds | Poucos: só um problema de ambiente (versão do `setuptools` desatualizada) e um engano de nome de ficheiro do binário — nada comparável à profundidade de debugging exigida pelo Zephyr. |
| Compatibilidade com caso de uso | **Forte.** OTA sem fios, fiável, completo, com rollback documentado — é o cenário exato que a Winegrid precisa. |
| Documentação / suporte Espressif | Excelente — é o SDK oficial do fabricante do chip, exemplos testados, documentação completa (README explica workflow, troubleshooting, produção). Contraste claro com o Zephyr, onde tivemos de investigar/descobrir várias coisas sem documentação direta. |

**Nota importante:** este resultado, obtido no dia seguinte ao crash de flash+Wi-Fi no Zephyr, usando a mesma placa e a mesma rede Wi-Fi, é uma forte evidência de que aquele problema **não era do hardware nem da rede** — era específico da implementação/coordenação flash+Wi-Fi no port do Zephyr para ESP32. O ESP-IDF, por ser o SDK nativo do fabricante, já resolve essa coordenação corretamente.
