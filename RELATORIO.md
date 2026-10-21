# Relatório Técnico – Pong Distribuído com ESP32

> Rascunho. Itens `[PREENCHER: medição]` dependem dos experimentos (ver EXPERIMENTOS.md). Referências de código: firmware = `_Projeto_Pong_Sugestao2.ino`; servidor = `nuvem/server/server.py`.

## 1. Visão geral

Dois ESP32 com MMA8452 enviam a posição da raquete por WebSocket a um servidor Python, que mantém o estado do jogo e o transmite a navegadores. Três camadas: embarcado, servidor, interface web.

## 2. Decisões de projeto

- **Aquisição:** 20 Hz (`vTaskDelay(pdMS_TO_TICKS(50))` em `taskSensor`). Suficiente para movimento humano de inclinação (poucos Hz) sem sobrecarregar I2C nem rede. `[PREENCHER: medição – taxa real]`
- **Tratamento do sinal:** eixo Y normalizado por limite de ±700 (contagens de 12 bits), saturado em [-1,1] e mapeado para [0,1] (`lerAcelerometroY`); média móvel de 20 amostras em buffer circular (`aplicarFiltro`).
- **Comunicação entre tarefas:** fila FreeRTOS de 10 posições (`sensorQueue`), produtor no Core 0 e consumidor no Core 1.
- **Protocolo:** JSON com `player`, `seq`, `ts`, `position` (ver README).
- **Servidor:** `asyncio` + `websockets`; um laço de física a 50 Hz (`executar_jogo`) e um handler por conexão.

## 3. Respostas às questões

**1. Onde existe paralelismo?** Em cada ESP32: `taskSensor` fixada no Core 0 e `taskNetwork` no Core 1 (`xTaskCreatePinnedToCore`) rodam fisicamente ao mesmo tempo. Os dois ESP32 também operam em paralelo entre si. No servidor não há paralelismo real: é um único processo/thread com `asyncio`.

**2. Onde existe concorrência?** (a) No ESP32, as duas tarefas competem pela fila. (b) No servidor, o `asyncio` intercala `receber_do_jogador` (×2), `receber_visualizador`, `executar_jogo` e `registrar_metricas`: concorrência cooperativa, sem execução simultânea. (c) No navegador, o recebimento de mensagens e o desenho.

**3. Onde existe processamento distribuído?** Em quatro máquinas distintas: dois ESP32, o servidor e o navegador, cooperando pela rede para uma única aplicação.

**4. Recursos compartilhados pelas tarefas do ESP32?** A fila `sensorQueue` (único dado compartilhado). Cada `Leitura` é copiada por valor para a fila. Os demais estados (buffer do filtro, contador `seq`) pertencem só à `taskSensor`; o objeto `webSocket` é usado só pela `taskNetwork`. O `Serial.print` é chamado só pela `taskSensor`.

**5. Há possibilidade de condição de corrida? Por quê?** Sim, se a posição fosse uma variável global `float` escrita por uma tarefa e lida pela outra sem proteção: a leitura poderia ocorrer no meio da escrita e produzir valor inconsistente, além de associar `position` a `seq`/`ts` de leituras diferentes (a struct tem três campos). Com a fila isso não acontece, pois a struct é copiada de forma atômica do ponto de vista das tarefas.

**6. Qual mecanismo foi escolhido?** Fila FreeRTOS (`xQueueCreate(10, sizeof(Leitura))`, `xQueueSend`, `xQueueReceive`), padrão produtor-consumidor.

**7. Por quê?** A fila já traz exclusão mútua e bloqueio internos, transporta a struct inteira (position+seq+ts) de forma consistente, desacopla as velocidades das tarefas e limita a memória usada. Um mutex em variável compartilhada guardaria só o valor mais recente e perderia `seq` consecutivos; a fila preserva a ordem.

**8. O que ocorre se a aquisição produzir mais rápido do que se transmite?** A fila (10 itens) enche. `xQueueSend` espera até 10 ms e, se não houver espaço, **descarta a leitura nova** (retorno ignorado), sem travar a aquisição. Consequência: o consumidor entrega leituras mais antigas primeiro, e o servidor pode receber atraso crescente (até ≈10 itens ≈ 0,5 s a 20 Hz). Observação: descartar a mais nova mantém dados velhos; para controle em tempo real seria preferível descartar a mais antiga (ou `xQueueOverwrite` numa fila de 1). `[PREENCHER: medição – se ocorreu descarte]`

**9. O que ocorre quando um ESP32 perde a conexão?** A `taskSensor` continua lendo normalmente, pois não depende da rede. A fila enche e as leituras novas passam a ser descartadas. No firmware da pasta `nuvem/firmware`, a `taskNetwork` só envia se `WiFi.status()` e `webSocket.isConnected()` forem verdadeiros; sem conexão ela continua esvaziando a fila e descarta o dado velho (contado em `leiturasDescartadas`), de modo que a aquisição nunca bloqueia. (O firmware original checava apenas `WiFi.status()`.) A biblioteca tenta reconectar a cada 5 s (`setReconnectInterval(5000)`). Se o Wi-Fi cair, o `WiFi.status()` impede o envio; a reconexão do Wi-Fi depende do comportamento padrão do núcleo ESP32 (não há `WiFi.reconnect()` explícito). No servidor, a conexão é encerrada e o jogo pausa.

**10. Como o servidor trata eventos dos dois jogadores?** Cada conexão tem um handler `async` (`receber_do_jogador`) que valida a mensagem (JSON, `seq` int crescente, `ts` e `position` numéricos finitos, `position` em [0,1]) e atualiza apenas o dicionário daquele jogador em `jogadores`. Ordem de chegada não importa porque os jogadores escrevem em campos diferentes. Uma nova conexão do mesmo jogador substitui a antiga.

**11. Quais informações formam o estado global?** Posições das raquetes (`jogadores[...]["position"]`), posição e velocidade da bola (`bola_x/y/dx/dy`), placar (`pontos_p1/p2`), vencedor, estado de conexão/atividade de cada jogador e o número de sequência dos estados.

**12. Como se mantém a consistência?** O servidor roda em uma única thread de evento (`asyncio`): as corrotinas só se alternam em pontos de `await`. `atualizar_fisica()` e `criar_estado()` são funções síncronas, logo cada estado publicado é um instantâneo coerente, sem locks. A lógica é única (uma instância do servidor); não há réplicas.

**13. Por que o estado fica no servidor e não no navegador?** Para haver uma única autoridade (evita divergência entre navegadores e entre jogadores), permitir que o jogo continue sem navegador, permitir múltiplos espectadores e impedir trapaça/inconsistência causada por cliente.

**14. O que ocorre quando o navegador desconecta?** Apenas a fila daquele visualizador é removida (`receber_visualizador`, bloco `finally`). A física e os jogadores continuam. Ao reconectar, o navegador recebe imediatamente o estado atual (`fila.put_nowait(criar_estado())`) e o `index.html` reconecta sozinho a cada 2 s.

**15. Impacto da latência sobre a jogabilidade?** O jogador vê a raquete atrasada em relação ao movimento físico, e as colisões usam a posição mais recente recebida (não a que o jogador "vê"). Com a bola a 5 px por tick de 20 ms (≈250 px/s), um atraso de 100 ms equivale a ≈25 px de erro, quase um quarto da raquete (100 px). `[PREENCHER: medição – latência fim a fim]`

**16. Partes com requisitos temporais mais críticos?** A cadeia sensor→servidor→tela (percepção de controle). O laço de física a 50 Hz (`INTERVALO_JOGO = 0.02`) deve manter cadência estável. A aquisição tem período fixo, mas tolera jitter; a rede é a parte menos previsível.

**17. A frequência de aquisição precisa igualar a de transmissão?** Não. Neste projeto coincidem (20 Hz) por simplicidade, mas poderiam diferir: amostrar mais rápido para filtrar melhor e enviar menos, ou o contrário. Se a aquisição for mais rápida que o envio, parte das leituras se acumula na fila ou é descartada; se for mais lenta, a rede fica ociosa. O que importa é a taxa de transmissão ser suficiente para o controle e a fila ter política de descarte adequada.

**18. Todos os dados do acelerômetro precisam chegar ao servidor?** Não. O servidor só usa a posição mais recente; perder amostras intermediárias é tolerável porque a posição é um valor de estado (não evento cumulativo) e a próxima leitura corrige. Já `seq` permite medir perdas. Entregar dados velhos (atrasados) é pior que perder.

**19. Todos os estados do servidor precisam chegar ao navegador?** Não. Cada visualizador tem fila de tamanho 1 (`asyncio.Queue(maxsize=1)`): se o navegador for lento, o estado antigo é substituído pelo novo (`publicar_estado`). O estado é um instantâneo completo, então estados perdidos não prejudicam a coerência.

**20. Mudanças para 100 ou 1.000 dispositivos?** Necessário: autenticação e identificação por dispositivo (hoje o caminho `/ws/playerN` é a identidade); salas/partidas isoladas em vez de estado global único; particionar o servidor em vários processos/instâncias atrás de balanceador com afinidade de sessão; broker (Redis/Kafka/MQTT) ou fan-out dedicado para espectadores; reduzir frequência e usar mensagens compactas (binário), possivelmente envio apenas de mudanças; TLS (`wss`); monitoramento e limites por conexão; o laço de física único passaria a ser um laço por partida, e CPU de um único processo Python passaria a ser o gargalo (`json.dumps` a 50 Hz por visualizador).

## 4. Limitações conhecidas (honestidade técnica)

- **Atraso do filtro:** média móvel de 20 amostras a 20 Hz representa janela de 1 s; o atraso médio de grupo é ≈ 0,5 s. Reduz ruído, mas prejudica a jogabilidade. Alternativa: janela de 4–8 amostras ou filtro exponencial.
- **Sem calibração/zona morta:** repouso depende de a protoboard estar nivelada; `limite_inclinacao = 700.0` é fixo.
- **Relógios não sincronizados:** `ts` é `millis()` do ESP32 (desde o boot) e `server_ts` é hora Unix do servidor; não se subtraem. `received_ts` − `server_ts` não mede latência de rede. Latência fim a fim exige método próprio (EXPERIMENTOS.md).
- **Sem TLS nem autenticação:** tráfego `ws://` em texto claro; qualquer cliente que conheça `/ws/player1` pode controlar uma raquete. Senha do Wi-Fi e endereço do servidor estão fixos no firmware.
- **Política de descarte:** fila cheia descarta a leitura mais nova (ver questão 8).
- **Detecção de desconexão no ESP32:** o firmware novo usa `isConnected()`; o original só checava o Wi-Fi (ver questão 9).
- **Servidor de instância única, thread única:** sem tolerância a falhas; se cair, a partida é perdida (estado só em memória).
- **Colisão simplificada:** só verifica a posição da bola no instante do tick; sem varredura contínua, a bola poderia atravessar a raquete em velocidades altas (não ocorre com 5 px/tick).

## 5. Resultados experimentais

| Experimento | Resultado |
|---|---|
| 1. Taxa de aquisição | [PREENCHER: medição] |
| 2. Taxa de comunicação | [PREENCHER: medição] |
| 3. Latência fim a fim | [PREENCHER: medição] |
| 4. Falha de comunicação | [PREENCHER: observações] |
| 5. Desconexão da interface | [PREENCHER: observações] |

## 6. Conclusão

[PREENCHER: após os experimentos]
