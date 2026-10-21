# Pong Distribuído com ESP32 e Processamento Paralelo

Dois ESP32, cada um com um acelerômetro MMA8452, controlam as raquetes de um Pong. O estado do jogo vive num servidor Python (WebSocket); o navegador só desenha o que o servidor envia.

```
ESP32 #1 (Core 0: sensor | Core 1: rede) ──ws /ws/player1──┐
                                                            ├─> Servidor Python ──ws /ws/viewer──> Navegador
ESP32 #2 (Core 0: sensor | Core 1: rede) ──ws /ws/player2──┘   (estado autoritativo)               (Canvas)
```

Documentos relacionados:
- **DEPLOY.md** – publicar o servidor na nuvem.
- **firmware/README_firmware.md** – gravar e configurar os ESP32.
- **RELATORIO.md** – rascunho do relatório técnico (20 questões).
- **EXPERIMENTOS.md** – roteiro dos 5 experimentos e da demonstração.

## Estrutura

```
nuvem/
├── server/server.py      # servidor: HTTP (página) + WebSocket + física do jogo
├── web/index.html        # interface (Canvas), só representa o estado recebido
├── firmware/             # código dos ESP32
├── requirements.txt      # websockets>=14.0
└── Dockerfile
```

## Componentes necessários

Por jogador: 1 ESP32, 1 MMA8452, 1 protoboard, 4 jumpers, alimentação USB (notebook ou power bank).
Além disso: 1 computador/servidor (local ou na nuvem) e uma rede Wi-Fi com acesso ao servidor (celular em hotspot funciona).

## Montagem (I2C MMA8452 ↔ ESP32)

O firmware usa `Wire.begin()` sem parâmetros, ou seja, os pinos I2C padrão do ESP32 e o endereço `0x1D` (`mma8452Address` em `initMMA8452()`).

| MMA8452 | ESP32 |
|---|---|
| VCC / 3V3 | 3V3 |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

Confirme na sua placa de breakout se o endereço é `0x1D` (pino SA0 em nível alto) ou `0x1C`; se for `0x1C`, altere `mma8452Address`. O eixo lido é o **Y** (registrador `0x03`); a raquete sobe/desce ao inclinar a protoboard nesse eixo.

## Bibliotecas

- Arduino IDE com o pacote de placas **ESP32** (Espressif).
- **WebSockets** de Markus Sattler (`WebSocketsClient.h`), instalável pelo Gerenciador de Bibliotecas.
- `Wire.h`, `WiFi.h` e FreeRTOS já vêm com o pacote ESP32.
- Servidor: Python 3.10+ e `pip install -r requirements.txt` (biblioteca `websockets`).

## Compilação e gravação dos ESP32

Resumo (detalhes em `firmware/README_firmware.md`):
1. Placa: "ESP32 Dev Module"; velocidade 115200 no monitor serial.
2. O mesmo firmware serve aos dois jogadores. Só mudam `PLAYER_ID` (`"player1"`/`"player2"`) e o caminho do WebSocket (`/ws/player1`/`/ws/player2`).
3. Ajuste SSID, senha e endereço/porta do servidor e grave em cada placa.

## Configuração de rede

- O ESP32 conecta no Wi-Fi (`WiFi.begin` em `taskNetwork`) e abre um WebSocket para `host:porta/caminho`.
- O servidor escuta em `0.0.0.0` na porta `8080` (ou na variável de ambiente `PORT`).
- ESP32 e servidor precisam se enxergar: na mesma rede local use o IP do PC; na nuvem use o IP público/domínio e libere a porta no firewall (ver DEPLOY.md).

## Inicializar o servidor local e abrir a interface

```
cd nuvem
pip install -r requirements.txt
python server/server.py
```

Abra **http://localhost:8080** no navegador. A página se conecta sozinha a `/ws/viewer` e reconecta se cair. Endpoints:

| Caminho | Uso |
|---|---|
| `/` | página do jogo |
| `/health` | resposta `ok` (verificação de saúde) |
| `/ws/player1`, `/ws/player2` | WebSocket de cada ESP32 |
| `/ws/viewer` | WebSocket do navegador (somente recebe) |

## Protocolo das mensagens

**ESP32 → servidor** (`payload` em `taskNetwork`, a cada leitura, ≈20 Hz):

```json
{"player":"player2","seq":1234,"ts":456789,"position":0.512}
```

| Campo | Significado |
|---|---|
| `player` | identificação informativa; o servidor identifica o jogador pelo **caminho** da conexão |
| `seq` | inteiro crescente por leitura; o servidor descarta `seq` ≤ último aceito (duplicada/fora de ordem) |
| `ts` | `millis()` do ESP32 na leitura (relógio local, não sincronizado com o servidor) |
| `position` | 0.0 a 1.0 (0.5 = centro); fora desse intervalo a mensagem é descartada |

O servidor (`receber_do_jogador`) também aceita um campo opcional `"type":"input"`; se vier com outro valor, a mensagem é ignorada.

**Servidor → navegador** (`criar_estado`, 50 vezes por segundo):

```json
{"type":"state","seq":812,"server_ts":1730000000000,"width":600,"height":400,
 "ball":{"x":300,"y":200,"radius":10,"visible":true},
 "players":{"player1":{"position":0.5,"connected":true,"active":true,
                       "last_seq":10,"last_ts":5000,"received_ts":1730000000000},
            "player2":{...}},
 "score":{"player1":0,"player2":0},"winner":null,"paused":false}
```

`active` = recebeu entrada nos últimos 2 s; `paused` = faltou algum jogador ativo (a física para).

## Execução completa

1. Subir o servidor (local ou nuvem).
2. Ligar os dois ESP32 (monitor serial mostra `Bruto/Filtrado`).
3. Abrir a página no navegador: "AGUARDANDO OS DOIS JOGADORES" some quando ambos enviam dados.
4. Inclinar as protoboards para mover as raquetes. Vence quem chegar a 15 pontos (`LIMITE_PONTOS`).
