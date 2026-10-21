import asyncio
import json
import math
import os
import time
from http import HTTPStatus
from pathlib import Path

import websockets
from websockets.exceptions import ConnectionClosed

# Porta vem da variável PORT (Render, Railway, Fly...) ou 8080 por padrão.
PORTA = int(os.environ.get("PORT", "8080"))
PAGINA = Path(__file__).parent.parent / "web" / "index.html"

LARGURA = 600
ALTURA = 400
ALTURA_RAQUETE = 100
RAIO_BOLA = 10
LIMITE_PONTOS = 15
INTERVALO_JOGO = 0.02  # 50 atualizações por segundo
TEMPO_SEM_ENTRADA = 2.0  # Segundos sem posição recebida antes de pausar
PAUSA_ENTRE_PARTIDAS = 8.0  # Segundos mostrando o vencedor antes de recomeçar

jogadores = {
    nome: {
        "ws": None,
        "position": 0.5,
        "seq": -1,
        "ts": None,
        "received_ts": None,
        "last_input": None,
        "messages": 0,
        "disconnects": 0,
    }
    for nome in ("player1", "player2")
}

visualizadores = set()
total_visualizadores_desconectados = 0

bola_x = LARGURA / 2
bola_y = ALTURA / 2
bola_dx = 5
bola_dy = 4
pontos_p1 = 0
pontos_p2 = 0
vencedor = None
instante_vitoria = 0.0
numero_estado = 0


def agora_ms():
    return int(time.time() * 1000)


def jogador_ativo(nome, instante):
    jogador = jogadores[nome]
    return (
        jogador["ws"] is not None
        and jogador["last_input"] is not None
        and instante - jogador["last_input"] <= TEMPO_SEM_ENTRADA
    )


def reiniciar_bola():
    global bola_x, bola_y, bola_dx, bola_dy

    bola_x = LARGURA / 2
    bola_y = ALTURA / 2
    bola_dx = -bola_dx
    bola_dy = 4 if bola_dy >= 0 else -4


def nova_partida():
    global pontos_p1, pontos_p2, vencedor

    pontos_p1 = 0
    pontos_p2 = 0
    vencedor = None
    reiniciar_bola()
    print("[Jogo] Nova partida", flush=True)


def registrar_vitoria(nome):
    global vencedor, instante_vitoria

    vencedor = nome
    instante_vitoria = time.monotonic()
    print(f"[Jogo] {nome} venceu; nova partida em {PAUSA_ENTRE_PARTIDAS:.0f} s", flush=True)


def atualizar_fisica():
    global bola_x, bola_y, bola_dx, bola_dy
    global pontos_p1, pontos_p2

    instante = time.monotonic()

    # Fim de partida: mostra o vencedor por um tempo e começa outra.
    if vencedor is not None:
        if instante - instante_vitoria >= PAUSA_ENTRE_PARTIDAS:
            nova_partida()
        return

    # Sem entradas recentes dos dois ESP32, a partida pausa.
    if not all(
        jogador_ativo(nome, instante) for nome in jogadores
    ):
        return

    bola_x += bola_dx
    bola_y += bola_dy

    if bola_y - RAIO_BOLA <= 0:
        bola_y = RAIO_BOLA
        bola_dy = abs(bola_dy)
    elif bola_y + RAIO_BOLA >= ALTURA:
        bola_y = ALTURA - RAIO_BOLA
        bola_dy = -abs(bola_dy)

    topo_p1 = jogadores["player1"]["position"] * (ALTURA - ALTURA_RAQUETE)
    topo_p2 = jogadores["player2"]["position"] * (ALTURA - ALTURA_RAQUETE)

    if (
        bola_dx < 0
        and bola_x - RAIO_BOLA <= 50
        and bola_x - RAIO_BOLA >= 30
        and topo_p1 <= bola_y <= topo_p1 + ALTURA_RAQUETE
    ):
        bola_x = 50 + RAIO_BOLA
        bola_dx = abs(bola_dx)

    elif (
        bola_dx > 0
        and bola_x + RAIO_BOLA >= 550
        and bola_x + RAIO_BOLA <= 570
        and topo_p2 <= bola_y <= topo_p2 + ALTURA_RAQUETE
    ):
        bola_x = 550 - RAIO_BOLA
        bola_dx = -abs(bola_dx)

    if bola_x + RAIO_BOLA < 0:
        pontos_p2 += 1
        if pontos_p2 >= LIMITE_PONTOS:
            registrar_vitoria("player2")
        else:
            reiniciar_bola()

    elif bola_x - RAIO_BOLA > LARGURA:
        pontos_p1 += 1
        if pontos_p1 >= LIMITE_PONTOS:
            registrar_vitoria("player1")
        else:
            reiniciar_bola()


def criar_estado():
    global numero_estado

    numero_estado += 1
    instante = time.monotonic()

    return {
        "type": "state",
        "seq": numero_estado,
        "server_ts": agora_ms(),
        "width": LARGURA,
        "height": ALTURA,
        "ball": {
            "x": bola_x,
            "y": bola_y,
            "radius": RAIO_BOLA,
            "visible": vencedor is None,
        },
        "players": {
            nome: {
                "position": jogador["position"],
                "connected": jogador["ws"] is not None,
                "active": jogador_ativo(nome, instante),
                "last_seq": jogador["seq"],
                "last_ts": jogador["ts"],
                "received_ts": jogador["received_ts"],
            }
            for nome, jogador in jogadores.items()
        },
        "score": {"player1": pontos_p1, "player2": pontos_p2},
        "winner": vencedor,
        "paused": vencedor is None and not all(
            jogador_ativo(nome, instante) for nome in jogadores
        ),
    }


def publicar_estado(estado):
    # Cada visualizador guarda só o estado mais recente. Um navegador lento
    # não bloqueia a física nem acumula estados antigos.
    for fila in tuple(visualizadores):
        if fila.full():
            fila.get_nowait()
        fila.put_nowait(estado)


async def enviar_ao_visualizador(websocket, fila):
    try:
        while True:
            estado = await fila.get()
            await asyncio.wait_for(
                websocket.send(json.dumps(estado)),
                timeout=2.0,
            )
    except (ConnectionClosed, TimeoutError):
        pass


async def receber_do_jogador(websocket, nome):
    jogador = jogadores[nome]

    # Uma reconexão substitui a conexão antiga daquele jogador.
    conexao_anterior = jogador["ws"]
    jogador["ws"] = websocket
    jogador["seq"] = -1
    jogador["last_input"] = None

    if conexao_anterior is not None and conexao_anterior is not websocket:
        await conexao_anterior.close(code=1000, reason="Nova conexão do jogador")

    print(f"[Rede] {nome} conectado")

    try:
        async for mensagem in websocket:
            if jogador["ws"] is not websocket:
                break

            try:
                dados = json.loads(mensagem)
                seq = dados["seq"]
                ts = dados["ts"]
                position = dados["position"]

                # "type" é opcional: o firmware atual não envia, mas se vier
                # precisa ser "input".
                if (
                    dados.get("type", "input") != "input"
                    or type(seq) is not int
                    or seq < 0
                    or type(ts) not in (int, float)
                    or not math.isfinite(ts)
                    or type(position) not in (int, float)
                    or not math.isfinite(position)
                    or not 0.0 <= position <= 1.0
                    or seq <= jogador["seq"]
                ):
                    continue
            except (ValueError, TypeError, KeyError, AttributeError):
                continue

            jogador["position"] = float(position)
            jogador["seq"] = seq
            jogador["ts"] = ts
            jogador["received_ts"] = agora_ms()
            jogador["last_input"] = time.monotonic()
            jogador["messages"] += 1

    except ConnectionClosed:
        pass
    finally:
        if jogador["ws"] is websocket:
            jogador["ws"] = None
            jogador["last_input"] = None
            jogador["disconnects"] += 1
            print(f"[Rede] {nome} desconectado; partida pausada")


async def receber_visualizador(websocket):
    global total_visualizadores_desconectados

    fila = asyncio.Queue(maxsize=1)
    visualizadores.add(fila)
    fila.put_nowait(criar_estado())
    print(f"[Rede] Visualizador conectado; total: {len(visualizadores)}")

    try:
        await enviar_ao_visualizador(websocket, fila)
    finally:
        visualizadores.discard(fila)
        total_visualizadores_desconectados += 1
        print(f"[Rede] Visualizador desconectado; total: {len(visualizadores)}")


async def handler(websocket):
    caminho = websocket.request.path.split("?", 1)[0]

    if caminho == "/ws/player1":
        await receber_do_jogador(websocket, "player1")
    elif caminho == "/ws/player2":
        await receber_do_jogador(websocket, "player2")
    elif caminho == "/ws/viewer":
        await receber_visualizador(websocket)
    else:
        await websocket.close(code=1008, reason="Endpoint desconhecido")


def responder_http(connection, request):
    """Requisições HTTP comuns: página do jogo e health check.
    Devolver None deixa o handshake WebSocket seguir normalmente."""
    caminho = request.path.split("?", 1)[0]

    if caminho.startswith("/ws/"):
        return None
    if caminho in ("/", "/index.html"):
        corpo = PAGINA.read_bytes()
        resposta = connection.respond(HTTPStatus.OK, corpo.decode("utf-8"))
        resposta.headers["Content-Type"] = "text/html; charset=utf-8"
        return resposta
    if caminho == "/health":
        return connection.respond(HTTPStatus.OK, "ok\n")
    return connection.respond(HTTPStatus.NOT_FOUND, "não encontrado\n")


async def executar_jogo():
    proxima_atualizacao = asyncio.get_running_loop().time()

    while True:
        atualizar_fisica()
        publicar_estado(criar_estado())

        proxima_atualizacao += INTERVALO_JOGO
        instante = asyncio.get_running_loop().time()

        # Se o servidor atrasar, retoma do instante atual em vez de
        # tentar enviar uma sequência de estados antigos.
        if proxima_atualizacao < instante:
            proxima_atualizacao = instante

        await asyncio.sleep(proxima_atualizacao - instante)


async def registrar_metricas():
    anteriores = {nome: 0 for nome in jogadores}

    while True:
        await asyncio.sleep(5)

        taxas = {}
        for nome, jogador in jogadores.items():
            recebidas = jogador["messages"] - anteriores[nome]
            taxas[nome] = round(recebidas / 5, 2)
            anteriores[nome] = jogador["messages"]

        print(json.dumps({
            "type": "metrics",
            "server_ts": agora_ms(),
            "accepted_messages_per_second": taxas,
            "player_disconnects": {
                nome: jogador["disconnects"]
                for nome, jogador in jogadores.items()
            },
            "viewer_disconnects": total_visualizadores_desconectados,
            "connected_viewers": len(visualizadores),
        }), flush=True)


async def main():
    async with websockets.serve(
        handler,
        "0.0.0.0",
        PORTA,
        process_request=responder_http,
        ping_interval=20,
        ping_timeout=20,
    ):
        print(f"Servidor Pong disponível na porta {PORTA}", flush=True)
        await asyncio.gather(executar_jogo(), registrar_metricas())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServidor encerrado")
