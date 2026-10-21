#include <Wire.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ==========================================
// [CLIENTE-SERVIDOR + SISTEMAS DISTRIBUÍDOS]
// Este ESP32 é o CLIENTE. O servidor na nuvem é o SERVIDOR remoto.
// São 2 dispositivos físicos + servidor = sistema distribuído.
// ==========================================

// ---------- CONFIGURAÇÃO (único bloco que muda entre as placas/ambientes) ----------
// TROCAR NO OUTRO ESP32: "player1" <-> "player2" (o path /ws/... acompanha sozinho)
// (no PlatformIO o valor vem do platformio.ini; o padrão abaixo vale para a Arduino IDE)
#ifndef PLAYER_ID
#define PLAYER_ID "player2"
#endif
#define WIFI_SSID "POCO X8 Pro"
#define WIFI_PASSWORD "123456789"
#define SERVER_HOST "SEU-SERVIDOR.exemplo.com" // domínio ou IP público do servidor na nuvem
#define SERVER_PORT 443                        // 443 com USE_SSL (wss); 8080 sem SSL (ws)
#define USE_SSL true                           // true = wss:// (HTTPS na nuvem), false = ws://
// -------------------------------------------------------------------------------

// [DOIS ENDPOINTS INDEPENDENTES] player1 usa "/ws/player1", player2 usa "/ws/player2"
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* websockets_server_host = SERVER_HOST;
const uint16_t websockets_server_port = SERVER_PORT;
const String websockets_server_path = String("/ws/") + PLAYER_ID;

WebSocketsClient webSocket;

// ==========================================
// [RECURSO COMPARTILHADO + PROTOCOLO DE COMUNICAÇÃO]
// Recurso compartilhado entre Core 0 e Core 1: a fila sensorQueue.
// Protocolo ESP32 -> Servidor (JSON via WebSocket):
// {"type":"input", "player":"player2", "seq":1234, "ts":456789, "position":0.512}
// - type: tipo da mensagem (hoje só "input")
// - player: distingue os 2 jogadores
// - seq: número de sequência para detectar perda e desordem
// - ts: millis() da leitura para medir atraso e frequência
// - position: 0.0 a 1.0, 0.5 = centro
// ==========================================
struct Leitura {
  float position;
  unsigned long seq;
  unsigned long ts;
};
QueueHandle_t sensorQueue; // [FILA + VARIÁVEL COMPARTILHADA PROTEGIDA]

// ==========================================
// [EXPERIMENTOS 1 E 2] contadores de taxa
// leiturasContadas: escrito só pela TaskSensor (Core 0)
// mensagensEnviadas: escrito só pela TaskNetwork (Core 1)
// Cada contador tem um único escritor e é lido apenas para imprimir
// estatística, então não precisa de mutex (volatile basta).
// ==========================================
volatile unsigned long leiturasContadas = 0;
volatile unsigned long mensagensEnviadas = 0;
volatile unsigned long leiturasDescartadas = 0; // dados velhos jogados fora sem conexão
const unsigned long INTERVALO_ESTATISTICA_MS = 5000;

const int mma8452Address = 0x1D;

void initMMA8452() {
  // [SISTEMAS EMBARCADOS] ESP32 lendo sensor físico via I2C
  Wire.begin();
  Wire.beginTransmission(mma8452Address);
  Wire.write(0x2A);
  Wire.write(0x01);
  Wire.endTransmission();
}

// [SEPARAÇÃO ESTADO / PROCESSAMENTO / APRESENTAÇÃO]
// Esta função é o ESTADO: lê o mundo físico, nada mais.
float lerAcelerometroY() {
  Wire.beginTransmission(mma8452Address);
  Wire.write(0x03); // Registro do eixo Y
  Wire.endTransmission(false);
  Wire.requestFrom(mma8452Address, 2);

  if (Wire.available() == 2) {
    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    int16_t yRaw = (int16_t)((msb << 8) | lsb);
    yRaw = yRaw / 16;

    float limite_inclinacao = 700.0;
    float normY = (float)yRaw / limite_inclinacao;

    // [TRATAMENTO DE FALHAS] corta valor absurdo
    if (normY > 1.0) normY = 1.0;
    if (normY < -1.0) normY = -1.0;

    return (normY + 1.0) / 2.0;
  }
  // [TRATAMENTO DE FALHAS] se sensor falhar, devolve centro para raquete não sumir
  return 0.5;
}

// ==========================================
// [BUFFER CIRCULAR EXPLÍCITO]
// Filtro de média móvel. Evita alocar memória nova.
//
// ATENÇÃO À LATÊNCIA: a aquisição roda a 20 Hz (50 ms por leitura).
// Uma média de N amostras atrasa a resposta em cerca de N * 50 ms / 2 no
// movimento contínuo e leva N * 50 ms para estabilizar após uma parada.
// Com 20 amostras isso dá ~1 s de atraso de estabilização: a raquete parece
// "pesada". Valores menores (ex.: 5 a 8, ~250-400 ms) reduzem a latência,
// ao custo de deixar passar mais ruído. Ajuste e meça (Experimento 3).
// ==========================================
#define NUM_LEITURAS 20
float leituras[NUM_LEITURAS];
int indice_leitura = 0;
float total_leituras = 0;
bool filtro_iniciado = false;

// [SEPARAÇÃO - PROCESSAMENTO] regra que limpa o sinal. Apresentação fica no servidor/tela.
float aplicarFiltro(float nova_leitura) {
  if (!filtro_iniciado) {
    for (int i = 0; i < NUM_LEITURAS; i++) {
      leituras[i] = nova_leitura;
    }
    total_leituras = nova_leitura * NUM_LEITURAS;
    filtro_iniciado = true;
  }
  total_leituras = total_leituras - leituras[indice_leitura];
  leituras[indice_leitura] = nova_leitura;
  total_leituras = total_leituras + leituras[indice_leitura];
  indice_leitura = (indice_leitura + 1) % NUM_LEITURAS; // anda em círculo
  return total_leituras / NUM_LEITURAS;
}

// ==========================================
// [PRODUTOR + PROGRAMAÇÃO CONCORRENTE]
// Tarefa infinita que produz dado o tempo todo sem esperar a rede.
// ==========================================
void taskSensor(void *pvParameters) {
  initMMA8452();
  static unsigned long contadorSeq = 0; // [NÚMERO DE SEQUÊNCIA] só aumenta
  unsigned long ultimaEstatistica = millis();
  unsigned long leiturasAnteriores = 0;
  unsigned long enviadasAnteriores = 0;
  unsigned long descartadasAnteriores = 0;

  while (1) {
    // [PROCESSAMENTO DE EVENTOS] cada inclinação é um evento
    float bruta = lerAcelerometroY();
    float filtrada = aplicarFiltro(bruta);

    Leitura l;
    l.position = filtrada;
    l.seq = contadorSeq++;
    l.ts = millis(); // [TIMESTAMP] hora da leitura para calcular atraso/frequência
    leiturasContadas++;

    Serial.print("Bruto:");
    Serial.print(bruta);
    Serial.print(", Filtrado:");
    Serial.println(filtrada);

    // [COMUNICAÇÃO ENTRE TAREFAS + SINCRONIZAÇÃO + EXCLUSÃO MÚTUA]
    // xQueueSend já tem mutex interno: só um núcleo escreve por vez.
    // Se a fila encher (produtor mais rápido que consumidor), espera 10ms e descarta o mais novo, não trava.
    // [CONDIÇÃO DE CORRIDA EVITADA] sem fila, o Core 1 poderia ler o float pela metade enquanto o Core 0 escreve.
    xQueueSend(sensorQueue, &l, pdMS_TO_TICKS(10));

    // [EXPERIMENTOS 1 E 2] taxas medidas em janela de 5 s
    unsigned long agora = millis();
    if (agora - ultimaEstatistica >= INTERVALO_ESTATISTICA_MS) {
      float janela = (agora - ultimaEstatistica) / 1000.0;
      unsigned long leituras_agora = leiturasContadas;
      unsigned long enviadas_agora = mensagensEnviadas;
      unsigned long descartadas_agora = leiturasDescartadas;
      Serial.print("[STATS] leituras/s=");
      Serial.print((leituras_agora - leiturasAnteriores) / janela, 2);
      Serial.print(" mensagens_enviadas/s=");
      Serial.print((enviadas_agora - enviadasAnteriores) / janela, 2);
      Serial.print(" descartadas_sem_conexao=");
      Serial.println(descartadas_agora - descartadasAnteriores);
      leiturasAnteriores = leituras_agora;
      enviadasAnteriores = enviadas_agora;
      descartadasAnteriores = descartadas_agora;
      ultimaEstatistica = agora;
    }

    vTaskDelay(pdMS_TO_TICKS(50)); // [TEMPO REAL + SINCRONIZAÇÃO] 20Hz
  }
}

// ==========================================
// [CONSUMIDOR + TCP/IP + WEBSOCKETS]
// ==========================================
void taskNetwork(void *pvParameters) {
  Leitura leituraRecebida;

  // [COMUNICAÇÃO TCP/IP] base de tudo, pega IP no Wi-Fi
  // Só esta tarefa (Core 1) espera pela rede; a aquisição segue lendo o sensor.
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // [WEBSOCKET SEGURO] wss:// quando o servidor está atrás de HTTPS (porta 443)
  if (USE_SSL) {
    webSocket.beginSSL(websockets_server_host, websockets_server_port, websockets_server_path.c_str());
  } else {
    webSocket.begin(websockets_server_host, websockets_server_port, websockets_server_path.c_str());
  }
  webSocket.setReconnectInterval(5000); // [FALHA DE COMUNICAÇÃO] reconecta sozinho a cada 5s

  while (1) {
    webSocket.loop(); // processa eventos de ida/volta sem travar

    // [SINCRONIZAÇÃO POR BLOQUEIO] se fila vazia, dorme 50ms em vez de gastar CPU
    if (xQueueReceive(sensorQueue, &leituraRecebida, pdMS_TO_TICKS(50)) == pdPASS) {
      // [FALHA DE COMUNICAÇÃO] a fila é SEMPRE drenada, mesmo sem conexão, para a
      // aquisição nunca ficar bloqueada. Sem conexão WebSocket o dado velho é
      // descartado: posição de raquete antiga não serve para jogar, o que vale é a mais nova.
      if (WiFi.status() == WL_CONNECTED && webSocket.isConnected()) {
        String payload = "{\"type\":\"input\",\"player\":\"" + String(PLAYER_ID) + "\",\"seq\":" + String(leituraRecebida.seq) + ",\"ts\":" + String(leituraRecebida.ts) + ",\"position\":" + String(leituraRecebida.position, 3) + "}";
        webSocket.sendTXT(payload);
        mensagensEnviadas++;
      } else {
        leiturasDescartadas++;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  // [FILA] buffer circular interno de 10 structs. É o único dado compartilhado.
  sensorQueue = xQueueCreate(10, sizeof(Leitura));

  // [PROGRAMAÇÃO MULTICORE + PARALELA] dois núcleos físicos em paralelo real
  // [PROGRAMAÇÃO CONCORRENTE] duas tarefas while(1) concorrendo
  // [PRODUTOR-CONSUMIDOR] Sensor produz no Core 0, Rede consome no Core 1
  xTaskCreatePinnedToCore(taskSensor, "TaskSensor", 4096, NULL, 2, NULL, 0);  // Produtor no Core 0
  xTaskCreatePinnedToCore(taskNetwork, "TaskNetwork", 8192, NULL, 2, NULL, 1); // Consumidor no Core 1 (pilha maior: TLS)
}

void loop() {
  // vazio porque tudo roda nas duas tasks do FreeRTOS
}
