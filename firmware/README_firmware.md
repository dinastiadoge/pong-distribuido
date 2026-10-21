# Firmware ESP32 — Pong Distribuído

Sketch: `pong_player/pong_player.ino` (o mesmo código para os dois jogadores).

## Requisitos
- Placa: **ESP32** (Arduino IDE → Placas → "ESP32 Dev Module", pacote *esp32 by Espressif*).
- Bibliotecas:
  - **WebSockets** by Markus Sattler (Library Manager → "WebSockets")
  - `Wire` e `WiFi` (já vêm com o pacote ESP32)
- Sensor MMA8452 no I2C (endereço `0x1D`), eixo Y.

## Configuração (bloco no topo do `.ino`)
| Constante | Significado |
|---|---|
| `PLAYER_ID` | `"player1"` ou `"player2"`. O path vira `/ws/player1` ou `/ws/player2` sozinho |
| `WIFI_SSID` / `WIFI_PASSWORD` | Rede Wi-Fi (precisa ter internet) |
| `SERVER_HOST` | Domínio ou IP público do servidor na nuvem (sem `http://`, sem porta) |
| `SERVER_PORT` | `443` com `USE_SSL true`; `8080` com `USE_SSL false` |
| `USE_SSL` | `true` = `wss://` (serviço com HTTPS), `false` = `ws://` (EC2 com IP e porta aberta) |

### Trocar de jogador
Grave a placa 1 com `PLAYER_ID "player1"` e a placa 2 com `PLAYER_ID "player2"`. Nada mais muda.

### Como achar o host
- **Serviço com HTTPS (Render, Railway, Fly...):** o domínio que o painel mostra, ex. `meu-pong.onrender.com`, com porta `443` e `USE_SSL true`.
- **VM (EC2, Oracle...):** o IP público da instância, com a porta em que o servidor escuta (ex. `8080`) liberada no firewall/Security Group e `USE_SSL false`.
- Teste antes de gravar: abra `http://HOST[:PORTA]/health` (ou `https://HOST/health`) no navegador; deve responder `ok`.

## Saída na serial (115200)
A cada 5 s: `[STATS] leituras/s=... mensagens_enviadas/s=... descartadas_sem_conexao=...`
— use para os Experimentos 1 (taxa de aquisição) e 2 (taxa de comunicação).

## Filtro e latência
`NUM_LEITURAS` (média móvel) vale 20 por padrão: com 20 Hz isso dá ~1 s de atraso de estabilização. Reduza para 5–8 se a raquete parecer lenta.
