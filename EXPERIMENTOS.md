# Roteiro de Experimentos e Demonstração

Pré-requisitos: servidor rodando, dois ESP32 conectados, navegador aberto, monitor serial (115200) aberto em pelo menos um ESP32. Anote data, rede (local ou nuvem) e versão do firmware.

## Experimento 1 – Taxa de aquisição (leituras/s)

**Como medir**
- Opção A (sem alterar código): no monitor serial, cada leitura imprime uma linha `Bruto:..., Filtrado:...` (`taskSensor`). Com timestamp do monitor (Arduino IDE: "Show timestamp"), conte as linhas em 30 s e divida por 30.
- Opção B: contar com `millis()` dentro da `taskSensor` e imprimir a cada 1 s (mudança no firmware, fora deste roteiro).

**Esperado:** ≈ 20/s (`vTaskDelay` de 50 ms, um pouco menos pelo tempo da leitura I2C e do `Serial.print`).

**Anotar:** quantidade de linhas, janela de tempo, taxa. Justifique: movimento humano de inclinação é lento (< 5 Hz); 20 Hz dá folga (Nyquist) sem sobrecarregar.

| Jogador | Linhas | Tempo (s) | Leituras/s |
|---|---|---|---|
| player1 | | | |
| player2 | | | |

## Experimento 2 – Taxa de comunicação (mensagens/s)

**Como medir:** o servidor imprime a cada 5 s uma linha JSON `"type":"metrics"` com `accepted_messages_per_second` por jogador (`registrar_metricas`). Deixe rodar 1 minuto e anote as médias. Confira o campo `last_seq` no estado ou o `seq` mais recente para estimar perdas: `perdidas = seq_final − seq_inicial − mensagens_aceitas`.

**Esperado:** ≈ 20/s por jogador (uma mensagem por leitura). Explique: poderia ser menor que a aquisição (menos tráfego) ou igual (menor latência); aqui é igual por simplicidade.

| Jogador | Msg aceitas/s (média) | seq inicial | seq final | Perdidas |
|---|---|---|---|---|
| player1 | | | | |
| player2 | | | | |

## Experimento 3 – Latência fim a fim

Os relógios do ESP32 (`millis()`) e do servidor não são sincronizados, então **não** subtraia `ts` de `server_ts`. Dois métodos viáveis:

**Método A – vídeo a 60 fps (recomendado, sem código novo)**
1. Posicione a protoboard e a tela do navegador no mesmo enquadramento de um celular filmando a 60 fps (ou 120 fps/câmera lenta).
2. Dê um movimento brusco e curto na protoboard (ex.: bater a mão ou inclinar de forma rápida).
3. No vídeo, conte os quadros entre o início do movimento da protoboard e o início do movimento da raquete na tela. Latência = quadros × 16,7 ms (a 60 fps).
4. Repita ≥ 10 vezes; reporte média, mínimo, máximo.
5. Lembre que o resultado inclui o atraso do filtro de 20 amostras (≈ 0,5–1 s), que domina. Se possível, repita com filtro menor e compare.

**Método B – ida e volta (RTT) rede+servidor**
Use o `ping` do sistema operacional para o IP do servidor, ou meça o tempo de abertura de uma conexão WebSocket de um terminal. Isso estima só a rede (ESP32→servidor ≈ RTT/2), não o sensor nem o navegador.

**Decomposição esperada** (para discutir): filtro + tempo de aquisição (50 ms) + fila + rede + física (≤ 20 ms) + rede de volta + desenho.

| Tentativa | Quadros | Latência (ms) |
|---|---|---|
| 1…10 | | |
| Média / mín / máx | | |

## Experimento 4 – Falha de comunicação

1. Com a partida rodando, desligue o Wi-Fi/hotspot de **um** ESP32 (ou desligue o roteador dele) por ~15 s.
2. Observe e anote:
   - **Tarefa de comunicação:** o que o monitor serial mostra; o ESP32 reconecta (a biblioteca tenta a cada 5 s)?
   - **Tarefa de aquisição:** as linhas `Bruto/Filtrado` continuam saindo na mesma cadência? (Este é o ponto-chave: a falha de rede não bloqueia o sensor.) Meça a taxa durante a falha como no Experimento 1.
   - **Servidor:** log `[Rede] playerN desconectado; partida pausada` e queda em `accepted_messages_per_second`.
   - **Outro jogador:** o jogo pausa; ele continua enviando e sua raquete segue ativa.
   - **Interface:** mostra "AGUARDANDO OS DOIS JOGADORES"; raquete do jogador caído fica cinza (`active:false`).
   - **Recuperação:** após religar, quanto tempo até o jogo retomar? Houve reinício da bola/placar? (O placar se mantém, pois está no servidor.)
3. Registre o tempo de queda, tempo de recuperação e se `seq` reiniciou.

| Item | Observação |
|---|---|
| Tarefa de comunicação | |
| Tarefa de aquisição (leituras/s durante a falha) | |
| Servidor | |
| Outro jogador | |
| Interface | |
| Tempo de recuperação (s) | |

## Experimento 5 – Desconexão da interface

1. Com partida em andamento e placar diferente de zero, anote o placar e feche a aba do navegador por ~10 s.
2. Observe o log do servidor (`Visualizador desconectado`) e que os ESP32 seguem enviando.
3. Abra a página de novo. Anote o placar e a posição da bola: o jogo continuou durante a ausência?
4. Conclusão a escrever: o estado reside no servidor (`bola_x/y`, `pontos_p1/p2`, `jogadores`); o navegador é só um espelho.

| Item | Observação |
|---|---|
| Placar antes / depois | |
| Servidor continuou rodando? | |
| Estado recebido ao reconectar | |

---

# Roteiro de demonstração

1. Mostrar o diagrama de arquitetura (README).
2. Subir o servidor; abrir a página (mostra "AGUARDANDO OS DOIS JOGADORES").
3. Ligar os ESP32; duas pessoas movem as protoboards; mostrar as raquetes respondendo.
4. Mostrar o monitor serial (Bruto vs Filtrado) e a linha `metrics` do servidor (≈ 20 msg/s por jogador).
5. Fechar e reabrir o navegador (Experimento 5).
6. Desligar o Wi-Fi de um ESP32 (Experimento 4) e religar.
7. Apontar no código: `xTaskCreatePinnedToCore`, `sensorQueue`, `receber_do_jogador`, `atualizar_fisica`.

## Perguntas prováveis e respostas curtas

- **O que acontece se eu desligar o Wi-Fi de um ESP32?** A leitura do sensor continua (tarefa independente); o servidor pausa o jogo por falta de entrada e retoma quando a conexão volta; o outro jogador não é afetado além da pausa.
- **E se eu fechar o navegador?** Nada muda no jogo; o estado está no servidor. Ao reabrir, a página recebe o estado atual e continua.
- **Onde está o paralelismo?** Sensor no Core 0 e rede no Core 1 do ESP32 (`xTaskCreatePinnedToCore`), mais os dois ESP32 em paralelo.
- **Como evitou condição de corrida?** Fila FreeRTOS entre as tarefas; nenhuma variável global compartilhada sem proteção.
- **Mudar a frequência de aquisição/envio?** Ajustar `vTaskDelay(pdMS_TO_TICKS(50))` em `taskSensor` (ex.: 25 ms = 40 Hz) e regravar. Se aumentar sem mudar o consumidor, a fila de 10 pode encher, mas a aquisição não trava.
- **Por que 20 Hz?** Movimento humano é lento; 20 Hz dá folga sem saturar rede/CPU.
- **Por que a raquete responde devagar?** Média móvel de 20 amostras ≈ 1 s de janela. Poderia ser reduzida.
- **E se a fila encher?** `xQueueSend` espera 10 ms e descarta a leitura nova; o sensor nunca bloqueia.
- **O navegador pode trapacear?** Não: só recebe. As raquetes vêm só dos ESP32, via `/ws/playerN`. Limitação: sem autenticação, qualquer cliente que conheça o caminho poderia se passar por um jogador.
- **Como o servidor sabe quem é quem?** Pelo caminho da conexão (`/ws/player1`, `/ws/player2`), tratados em `handler`.
- **Dois jogadores enviam ao mesmo tempo, há conflito?** Não: cada um escreve no próprio registro em `jogadores`, e o `asyncio` executa um handler por vez.
- **Por que o servidor não envia todo estado ao navegador lento?** Fila de tamanho 1 por visualizador: só vale o estado mais recente.
- **E com 1.000 dispositivos?** Ver questão 20 do RELATORIO.md (salas por partida, vários processos, broker, TLS, autenticação).
- **Latência medida?** [PREENCHER: resultado do Experimento 3].
