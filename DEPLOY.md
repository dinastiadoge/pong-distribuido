# Colocando o Pong na nuvem (sem conta AWS própria)

O servidor (`server/server.py`) serve a página (`/`), o health check (`/health`) e os
WebSockets (`/ws/player1`, `/ws/player2`, `/ws/viewer`) **na mesma porta**, lida da
variável `PORT` (padrão 8080). Qualquer lugar que rode Python ou Docker serve.

> Os limites abaixo vêm de pesquisa na web em out/2026 e mudam com frequência.
> Confirme na página oficial de cada serviço antes de depender de um número.

## Comparativo

| Opção | Custo | Cartão? | Dorme por inatividade? | WebSocket | Observações |
|---|---|---|---|---|---|
| **Render (free)** ⭐ | grátis | em geral não (confirme no cadastro) | Sim, após 15 min **sem tráfego**; desde fev/2026 mensagens WebSocket contam como atividade | Sim | Deploy direto do GitHub, HTTPS/`wss://` pronto. Acordar leva ~1 min. |
| **AWS Academy Learner Lab** (reserva) | grátis (créditos da faculdade) | não | Lab dura ~4 h por sessão; instâncias param quando acaba | Sim | Precisa de convite do professor. IP público muda se não usar Elastic IP. Só `us-east-1` e `us-west-2`. |
| Oracle Cloud Always Free | grátis | **sim** (verificação) | não, mas reclama VMs ociosas | Sim | Falta de capacidade frequente; cadastro às vezes rejeita cartões. |
| Fly.io | pago / teste curto | sim | configurável | Sim | Fontes divergem sobre o plano grátis; provavelmente exige cartão. |
| Railway | teste com crédito, depois ~US$5/mês | verificar | não | Sim | Crédito grátis acaba em pouco tempo. |
| Google Cloud e2-micro | grátis (Always Free) | **sim** | não | Sim | Exige cartão e configurar firewall/VM manualmente. |

## Recomendação

1. **Principal: Render free.** Não depende da AWS, não precisa de SSH, dá URL com HTTPS.
   Os ESP32 mandam ~20 mensagens/s, então o serviço não dorme durante a partida.
2. **Reserva: EC2 no AWS Academy Learner Lab** (a seção "Pelo menos um aluno com AWS"
   do enunciado sugere que a turma tem acesso — pergunte ao professor/colegas).

Pontos de atenção em ambos:

- **Hotspot do celular:** funciona. O ESP32 só faz conexão de saída para a internet.
- **Primeira conexão após dormir (Render):** abra a URL no navegador 1–2 min antes da
  demonstração para "acordar" o serviço e deixe o `/health` respondendo.
- **Estado em memória:** reiniciar/redeploy zera o placar. É esperado (estado vive no servidor).

---

## Passo a passo A — Render (recomendado)

### 1. Subir o código para o GitHub
Na pasta `nuvem/` (sem a chave `.pem`!):

```powershell
cd F:\Faculdade\CP\_Projeto_Pong_Sugestao2\nuvem
git init
git add .
git commit -m "Pong distribuido"
git branch -M main
git remote add origin https://github.com/SEU_USUARIO/pong-distribuido.git
git push -u origin main
```

(Crie antes o repositório vazio em github.com → New repository.)

### 2. Criar o serviço no Render
1. Entre em <https://render.com> com a conta do GitHub.
2. **New → Web Service** → escolha o repositório → Runtime **Docker**, Instance type **Free**.
3. *Health Check Path*: `/health`. Clique **Create Web Service**.
   - Alternativa: **New → Blueprint** usando o `render.yaml`. Como o repositório acima é só
     o conteúdo de `nuvem/`, **apague a linha `rootDir: nuvem`** do `render.yaml` antes.
4. Aguarde o build. A URL será algo como `https://pong-distribuido.onrender.com`.

### 3. Testar
```bash
curl https://pong-distribuido.onrender.com/health      # deve responder: ok
```
Abra a URL no navegador do computador **e do celular** (4G, fora do Wi-Fi do PC): deve
aparecer a arena com "AGUARDANDO OS DOIS JOGADORES".

### 4. Apontar os ESP32
No firmware, o servidor passa a ser **wss na porta 443**:

```cpp
const char* websockets_server_host = "pong-distribuido.onrender.com";
const uint16_t websockets_server_port = 443;
// e trocar webSocket.begin(...) por:
webSocket.beginSSL(websockets_server_host, websockets_server_port, websockets_server_path);
```

Observações: use só o domínio (sem `https://`). O `beginSSL` da biblioteca
`WebSocketsClient` sem fingerprint/certificado **não valida** o certificado do servidor —
aceitável para o trabalho, mas cite isso no relatório (limitação de segurança).
Regrave as duas placas (`player1` / `/ws/player1` e `player2` / `/ws/player2`).

### 5. Ver logs (para os experimentos)
Painel do serviço → **Logs**. A cada 5 s o servidor imprime as métricas JSON
(mensagens aceitas/s de cada player, desconexões e visualizadores) — use para os
Experimentos 2, 4 e 5.

---

## Passo a passo B — EC2 no AWS Academy Learner Lab (reserva)

### 1. Entrar e iniciar o lab
1. Acesse o curso do **AWS Academy Learner Lab** pelo link/convite da faculdade → **Módulos → Learner Lab**.
2. Clique **Start Lab**; espere a bolinha ao lado de **AWS** ficar verde → clique em **AWS** (abre o console).
3. A sessão dura cerca de 4 h; reiniciar o lab zera o contador.

### 2. Criar a instância
No console (região `us-east-1`): **EC2 → Launch instance**
- Nome: `pong`; AMI: **Ubuntu Server 22.04/24.04**; tipo: `t2.micro`/`t3.micro`.
- Key pair: crie um novo (`.pem`) e **guarde fora do repositório**.
- Network settings → **Edit** → Security group com regras de entrada:
  - SSH (22) — My IP
  - **Custom TCP 8080 — 0.0.0.0/0** (jogadores e navegadores)
- **Launch instance**.

### 3. IP fixo (recomendado)
**EC2 → Elastic IPs → Allocate → Associate** com a instância. Sem isso o IP público muda
sempre que a instância parar (e o firmware teria de ser regravado). Libere o Elastic IP
quando terminar o trabalho.

### 4. Instalar e subir o servidor
Do seu PC (troque o caminho do `.pem` e o IP):

```powershell
scp -i C:\caminho\pong.pem -r F:\Faculdade\CP\_Projeto_Pong_Sugestao2\nuvem ubuntu@IP_PUBLICO:~/pong
ssh -i C:\caminho\pong.pem ubuntu@IP_PUBLICO
```

Dentro da instância:

```bash
sudo mkdir -p /opt/pong && sudo cp -r ~/pong/* /opt/pong/ && sudo chown -R ubuntu:ubuntu /opt/pong
sudo apt-get update && sudo apt-get install -y python3-venv
python3 -m venv /opt/pong/venv
/opt/pong/venv/bin/pip install -r /opt/pong/requirements.txt
sudo cp /opt/pong/server/pong.service /etc/systemd/system/pong.service
sudo systemctl daemon-reload
sudo systemctl enable --now pong
systemctl status pong --no-pager
```

Logs e métricas: `journalctl -u pong -f`

### 5. Testar
```bash
curl http://IP_PUBLICO:8080/health      # ok
```
Navegador (PC e celular): `http://IP_PUBLICO:8080/`.

### 6. Firmware (ws, sem TLS)
```cpp
const char* websockets_server_host = "IP_PUBLICO";
const uint16_t websockets_server_port = 8080;
webSocket.begin(websockets_server_host, websockets_server_port, websockets_server_path);
```

Se o lab for reiniciado e o serviço não subir sozinho, o `systemd` (enable) o inicia no
boot; confirme com `systemctl status pong`.

---

## Problemas comuns

| Sintoma | Causa provável |
|---|---|
| `curl /health` não responde na EC2 | Security group sem a porta 8080, ou serviço parado (`systemctl status pong`). |
| ESP32 não conecta no Render | Usando `begin` em vez de `beginSSL`, ou porta ≠ 443, ou `https://` no host. |
| Primeira conexão demora ~1 min (Render) | Serviço dormindo; abra a URL antes da apresentação. |
| Página abre mas fica em "Conectando" | Proxy/rede bloqueando WebSocket; teste no 4G. |
| IP da EC2 mudou | Não usou Elastic IP; atualize o firmware. |

## Fontes
- Render — [WebSocket mantém serviço free ativo (changelog, fev/2026)](https://render.com/changelog/free-web-services-now-remain-active-while-receiving-websocket-messages)
- [Tutorial CS559 — AWS Learner Lab (sessão de 4 h, Elastic IP)](https://www.csd.uoc.gr/~hy559/fall23/CS559-AWS_Tutorial.pdf)
- [AWS Academy Learner Lab — serviços e regiões](https://cyberlab.pacific.edu/courses/comp175/resources/aws-academy/AWS_Academy_Learner_Lab-Foundational_Services.pdf)
- [Plataformas com plano grátis em 2026 (Render)](https://render.com/articles/platforms-with-a-real-free-tier-for-developers-in-2026.md)
