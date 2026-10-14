# Horta Inteligente (ESP32-S3)

Monitor de horta hidropônica: temperatura da solução, fluxo da bomba, nível do reservatório, alertas no Telegram e painel web local.

## Arquivos
- `horta_inteligente.ino`: firmware
- `Index.html`: painel web (fonte editável)
- `web_page.h`: `Index.html` compactado (gzip) e embutido no firmware. Gerado por `python3 gerar_web_page.py`; rode de novo sempre que editar o HTML.
- `gerar_web_page.py`: gera `web_page.h` a partir de `Index.html`.
- `simulador_painel.py`: serve o painel com dados simulados (7 dias de histórico, reabastecimentos e eventos) para testar sem o ESP32. Serve o `Index.html` direto, relido a cada recarga da página.
- `icones.h`: bitmaps do OLED (ícones, WiFi, arte da abertura). Gerado, não edite à mão.
- `gerar_icones.py`: gera `icones.h`. Para alterar um ícone, edite o script e rode `python3 gerar_icones.py`.
- `tools/`: teste da lógica de eventos que roda no computador (`rodar_teste_eventos.py`).
- `preview_oled.py`: simula as telas do OLED e salva `preview_telas.png`, para conferir sem gravar no ESP32.
- `credenciais.h`: tokens e senhas (Telegram, painel, tomada, Firebase). Fica só no seu computador e está no `.gitignore`. Modelo sem segredos: `credenciais.exemplo.h`.
- `firebase/database.rules.json`: regras do Realtime Database (cole no console do Firebase).
- `painel_nuvem/`: o que vai para o GitHub Pages. O `index.html` é a cópia do painel gerada pelo `gerar_web_page.py`. O `manifest.webmanifest`, o `sw.js` e os ícones fazem dele um app instalável (PWA).
- `gerar_icones_app.py`: gera os ícones do app em `painel_nuvem/` (Python puro, sem PIL).
- `painel_nuvem/insights/` e `painel_nuvem/.github/workflows/insights.yml`: geração diária dos insights com IA (ver abaixo).

Todos os arquivos `.h` ficam na mesma pasta do `.ino`.

## Pinagem
| Função | GPIO |
|---|---|
| Encoder CLK / DT / SW | 15 / 16 / 17 |
| OLED SDA / SCL | 9 / 10 |
| Fluxo YF-S201 | 47 |
| Nível XKC-Y25-V | 48 |
| DS18B20 | 4 (resistor de 4,7 kΩ para 3V3) |
| Reserva | 36 |

## Bibliotecas
Adafruit GFX, Adafruit SSD1306, WiFiManager, UniversalTelegramBot, ArduinoJson 7, ESPAsyncWebServer, OneWire, DallasTemperature.

## Uso
1. No primeiro boot, conecte-se à rede `Horta-Setup` e configure o WiFi.
2. Na tela de início, escolha Modo Normal ou Manutenção com o encoder.
3. Acesse o IP exibido no painel pelo navegador. Alterações pedem login (`WEB_USER` / `WEB_PASS` no firmware).
4. Comandos do Telegram: `/status` (estado atual) e `/resumo` (resumo das últimas 24 h).
5. Clique curto no encoder envia o status ao Telegram. Clique longo (2 s) alterna o modo manutenção.

## Configuração
Todas as credenciais ficam em `credenciais.h` (copie de `credenciais.exemplo.h` numa instalação nova). Nunca publique esse arquivo.

## Acesso de qualquer lugar (Firebase + GitHub Pages)
O painel publicado no GitHub Pages (https) não alcança o ESP32 na rede de casa. Os dois se encontram no Firebase Realtime Database (plano gratuito), sem abrir portas no roteador:

| Caminho no banco | Quem escreve | Conteúdo |
|---|---|---|
| `/horta/doEsp/estado` | ESP32 | o mesmo JSON de `/api/data`, mais `vistoEm` (hora do servidor) |
| `/horta/paraEsp/req/{id}` | painel | pedido de uma rota: `{url, metodo, criado}` |
| `/horta/doEsp/resp/{id}` | ESP32 | resposta da rota: `{codigo, tipo, corpo}` |
| `/horta/paraEsp/presenca` | painel | hora do servidor, renovada a cada 20 s enquanto o painel está aberto |

O ESP32 executa as **mesmas rotas** do painel local (tabela `ROTAS_API`), então nada é duplicado. Com o painel aberto, ele consulta os pedidos a cada 1,5 s e publica o estado a cada 5 s. Fechado, a cada 5 s e 60 s. Pedidos com mais de 2 min são descartados (ex.: um "reiniciar" enviado enquanto a horta estava fora do ar). As regras só deixam o seu usuário ler e pedir, e o usuário do ESP32 só publica o estado e as respostas.

A mesma página funciona nos dois modos: servida pelo ESP32 (`http://`), fala direto com `/api/*`. Servida por `https://` (ou com `?nuvem=1` no endereço), pede login e usa o Firebase.

### Configurar
1. Console do Firebase › Realtime Database › Regras: cole o conteúdo de `firebase/database.rules.json` e publique.
2. Em `credenciais.h`, preencha `FIREBASE_SENHA` com a senha do usuário `esp32@gmail.com`. Vazia, a nuvem fica desligada.
3. Teste sem o ESP32: `python3 simulador_painel.py --nuvem` e abra `http://localhost:8080/?nuvem=1`. Entre com a sua conta. O simulador faz o papel do ESP32 pelo Firebase.
4. Rode `python3 gerar_web_page.py` e suba **todo o conteúdo** de `painel_nuvem/` para um repositório público com o GitHub Pages ligado.
5. Console do Firebase › Authentication › Settings › Authorized domains: adicione `seu-usuario.github.io`.
6. Grave o firmware. Em Ajustes › Dispositivo, a linha **Nuvem** mostra o estado da conexão do ESP32.

### Insights com IA (aba Gráficos)
Duas vezes por dia (07:17 e 21:17, horário de Brasília) um trabalho agendado do GitHub Actions gera uma análise da horta e grava em `/horta/insights` no Firebase. O painel online mostra na aba Gráficos: resumo e humor do dia, insights com severidade e confiança (toque para ver os números usados), tendências, ações sugeridas, previsão de calor/chuva e o histórico das análises anteriores.

Como funciona (`painel_nuvem/insights/gerar_insights.py`, só biblioteca padrão do Python):
1. Pede ao ESP32, pelo Firebase, o histórico de 7 dias, o das últimas 24 h, os eventos e os reabastecimentos (o mesmo mecanismo do painel, só leitura).
2. Busca clima e previsão no Open-Meteo e calcula a fase da lua.
3. **Calcula em código** as estatísticas: médias por dia, tendência de vazão, estimativa da temperatura máxima da solução nos próximos dias (regressão com a máxima do ambiente), previsão do mínimo do reservatório (evapotranspiração acumulada) e alertas objetivos.
4. Manda esses números e o `perfil_horta.md` ao Gemini, que só interpreta e escreve. A resposta é validada antes de ser gravada.
5. Guarda um resumo diário em `/horta/resumos` para tendências de longo prazo.

Se a horta estiver sem comunicação, a análise sai só com clima e previsão e avisa isso. Se a geração parar por mais de 20 h, o painel mostra um aviso.

**Configurar** (uma vez):
1. Firebase › Authentication: crie o usuário `insights@…` (e-mail qualquer, senha própria) e copie o UID para `firebase/database.rules.json` (troque `COLE_AQUI_O_UID_DO_INSIGHTS`). Publique as regras. Esse usuário só pode pedir leitura (`GET`) das rotas de consulta.
2. Google AI Studio: crie uma chave da API do Gemini.
3. GitHub › repositório › Settings › Secrets and variables › Actions › New repository secret: `INSIGHTS_EMAIL`, `INSIGHTS_SENHA`, `GEMINI_API_KEY`, `HORTA_LAT`, `HORTA_LON` (as coordenadas ficam em segredo porque o repositório é público). Opcional, em Variables: `GEMINI_MODEL` (padrão `gemini-flash-latest`).
4. Suba para o repositório: `index.html`, `insights/gerar_insights.py`, `insights/perfil_horta.md` e `.github/workflows/insights.yml`.
5. Aba Actions › Insights da horta › Run workflow, para testar sem esperar o horário.

Edite o `insights/perfil_horta.md` com as culturas, o local e o que importa para você: a IA lê esse arquivo a cada geração.

**Testar sem Firebase nem Gemini**: `python3 painel_nuvem/insights/gerar_insights.py --fonte-local http://localhost:8080 --saida saida.json --ia-falsa --mostrar-dados` (com `HORTA_LAT` e `HORTA_LON` definidos). O simulador também serve `/api/insights` com uma análise de exemplo para ver a aparência da seção.

Custos: o plano gratuito do Gemini Flash cobre 2 chamadas por dia com folga. Com uma chave de projeto **com faturamento ativo**, cada chamada é cobrada (centavos por mês neste uso; confira o preço atual). O GitHub Actions é gratuito em repositório público, e o Firebase usa menos de 1 MB a mais.

### App no celular (PWA)
Aberto pelo GitHub Pages, o Chrome oferece **Instalar app**, pelo menu ⋮ ou pelo botão em Ajustes › Dispositivo. O app ganha ícone próprio e abre em tela cheia, sem a barra do navegador. A página sempre vem da rede, com a versão mais nova. Sem internet, abre a última cópia guardada. O SDK do Firebase fica guardado no aparelho, então o app abre mais rápido. No iPhone, use Safari › Compartilhar › Adicionar à Tela de Início.

## Interface do OLED
- Ícones grandes em grade única de 25x25 px (pixel central real), centrados em x=31 e x=95, simétricos ao divisor (x=63).
- Todos os ícones têm animação ligada ao estado: termômetro (nível do mercúrio acompanha a temperatura), gota (ondas correm com fluxo; parada, só contorno), reservatório (água ondula; nível mínimo pisca o "!") e bomba (hélice gira ligada; parada, o cubo pisca).
- Ícone de WiFi no eixo do divisor; sem conexão, os arcos surgem em sequência.

## Painel web
Cinco abas, no estilo dos apps do iPhone (tema claro/escuro automático, funciona sem internet: gráficos em SVG próprio).
- **Resumo**: status geral, bomba com anel de progresso do ciclo e faixa liga/desliga das últimas 6 h, temperaturas, vazão e reservatório com mini-gráficos. No celular em pé ocupa exatamente a tela, sem rolagem: os mini-gráficos esticam ou encolhem conforme a altura. Só rola quando nem o mínimo cabe (celular deitado). Com várias panes, o status mostra a contagem, e o toque abre a aba Alertas.
- Nas abas que rolam, um indicador fino na borda direita aparece durante a rolagem e pode ser arrastado (telas de toque).
- **Gráficos** (6 h / 24 h / 7 d): tempo ligada da bomba e disponibilidade da tomada, ciclos (quantidade e duração); vazão média com faixa mínima–máxima e a vazão mínima configurada; temperatura da solução × ambiente no mesmo eixo, com a diferença. Toque ou passe o mouse para ver os valores.
- **Alertas**: central de eventos com selo de não lidos. Mostra panes (início, duração, se normalizou sozinha ou foi limpa à mão), reinícios com o motivo (queda de tensão, watchdog, travamento), manutenção, reabastecimentos, ajustes salvos e quedas de WiFi, agrupados por dia, com resumo dos últimos 7 dias.
- **Solução**: há quanto tempo a solução atual está em uso, previsão do próximo reabastecimento (pela média), duração de cada solução e tempo até o nível mínimo.
- **Ajustes**: parâmetros, manutenção, Telegram e dados do dispositivo.

## Histórico
- Uma amostra por minuto (temperaturas, vazão média/mín/máx, segundos de bomba ligada e de tomada online, alertas). Guarda 7 dias com PSRAM habilitada (`Tools > PSRAM: OPI PSRAM`), ou 24 h sem ela.
- Gravado em `/hist.bin` no LittleFS a cada 10 min, para sobreviver a quedas de energia. Precisa de um esquema de partição com área de arquivos (ex.: `16M Flash (3MB APP/9.9MB FATFS)`); sem ela, o histórico fica só na RAM.
- Só começa a gravar depois que o relógio sincroniza pelo NTP.

### Reabastecimentos
Cada reabastecimento inicia uma "solução". Ele é registrado:
- automaticamente quando o nível sai do mínimo;
- ao destravar o alerta de nível pelo painel (se estava no mínimo);
- pelo botão **Registrar reabastecimento** (para quando você completa o tanque antes do mínimo).
Os últimos 60 registros (~1 ano com reabastecimentos semanais) ficam nas Preferences (namespace `horta_sol`), separados do histórico de 7 dias.

### Resumo diário
Uma mensagem por dia no Telegram, no horário escolhido em **Ajustes › Resumo diário** (padrão 20:00), com as últimas 24 h: tempo de bomba ligada e ciclos, vazão média e faixa, temperaturas da solução e do ambiente, há quanto tempo a solução está em uso com a previsão do próximo reabastecimento, panes (quantidade e duração), reinícios inesperados e quedas de WiFi. O painel mostra uma prévia e tem o botão **Enviar resumo agora**. Se o ESP32 estiver desligado na hora marcada, o resumo daquele dia não é enviado.

### Mensagens do Telegram
Todas usam o formato HTML do Telegram (negrito nos títulos, `código` nos valores, itálico nas explicações), com separadores e barras de progresso (▰▰▰▱▱):
- **Status** (`/status`, botão "📊 Status" e relatório horário): situação geral, panes ativas com o tempo de cada uma, bomba, vazão, temperaturas, reservatório e idade da solução.
- **Alerta de pane**: severidade no emoji, o que aconteceu com os valores reais, desde que horas e possíveis causas. O **lembrete** repete com "ativa há…".
- **Resolvido**: quanto tempo durou e se normalizou sozinha ou foi limpa à mão.
- **Resumo do dia** (`/resumo`, botão "📈 Resumo"): bomba, vazão, temperaturas, reservatório e ocorrências.
- Um teclado fixo no rodapé do chat (📊 Status / 📈 Resumo) e o menu `/` com os comandos são configurados automaticamente.
- Se o Telegram recusar o HTML (erro 400), a mensagem é reenviada em texto puro, então nenhum alerta se perde.
- Em **Ajustes › Resumo diário › Ver prévia das mensagens** dá para ver cada tipo, com o mesmo visual do Telegram, sem esperar uma pane acontecer. A prévia vem de `GET /api/msg?tipo=status|alerta|resolvido|resumo&id=N`.

### Central de eventos
Os últimos 512 eventos ficam em `/eventos.bin` no LittleFS, gravados na hora em que acontecem. Eventos anteriores ao NTP recebem o horário assim que o relógio sincroniza.

### Travamentos e modo seguro
- **Reinícios seguidos viram um evento só.** Se o ESP32 trava ao iniciar várias vezes, a central mostra "Travamento em loop: reiniciou 412 vezes seguidas", em vez de uma linha por volta. O horário do evento é o do último reinício e é preenchido assim que o relógio sincroniza.
- **Migração automática.** No primeiro boot com este firmware, sequências antigas de travamentos sem horário (rastro de um loop) são agrupadas em um evento.
- **Contagem que sobrevive aos travamentos.** O número de travamentos seguidos fica na memória RTC e zera depois de 60 s de funcionamento estável, ou ao ligar a energia, apertar o RST ou regravar.
- **Modo seguro nível 1 (3 travamentos seguidos):** o OLED e o barramento I2C não são iniciados. **Nível 2 (6 seguidos):** também ficam desligados o histórico (PSRAM e flash) e a central de eventos. O WiFi, o painel, o Telegram, os sensores e as panes continuam. Os reabastecimentos registrados não são perdidos.
- O painel mostra um aviso na tela Resumo, o Telegram avisa com som no boot e o resumo diário cita o modo seguro. **Ajustes › Dispositivo › Reiniciar horta** tenta o modo normal.
- O resumo diário só conta eventos que têm horário. A bomba, controlada pela tomada Tuya, não depende do ESP32.
- **Ajustes › Dispositivo › Limpar central de alertas** apaga todo o histórico de eventos (as panes em andamento continuam aparecendo).
- Teste no computador da lógica de agrupar e compactar: `python3 tools/rodar_teste_eventos.py`.

### Desempenho
- A página vai em gzip (~24 KB em vez de ~90 KB) com ETag: depois da primeira visita o navegador só confirma que não mudou.
- O painel consulta `/api/data` (~1 KB) a cada 2 s; histórico e eventos só quando a aba é aberta ou quando surge evento novo.
- Em **Ajustes > Dispositivo** aparecem a memória livre, o sinal WiFi e a volta mais lenta do loop no último minuto (inclui o tempo de atender o painel).
- Acesso pelo nome: `http://horta.local` (mDNS).

### Rotas da API
| Rota | Descrição |
|---|---|
| `GET /api/data` | estado atual (inclui valores numéricos `tSol`, `tAmb`, `fluxo`, `tomadaSeg`, `epoch`) |
| `GET /api/history?min=1440&pts=288` | histórico agrupado em até `pts` pontos + estatísticas do período |
| `GET /api/solucoes` | reabastecimentos (o último é a solução atual) |
| `GET /api/eventos?n=150` | eventos, do mais novo ao mais antigo: `[seq, epoch, tipo, id, duração, extra]` |
| `GET /api/msg?tipo=status\|alerta\|resolvido\|resumo&id=0` | mensagem do Telegram em HTML (prévia) |
| `POST /api/action?cmd=reabastecido` | registra um reabastecimento (pede login) |
| `POST /api/action?cmd=reabastecidoEm&ts=<epoch>` | lança um reabastecimento que já aconteceu (só datas depois do último registro; não cria evento na central de alertas) |
| `POST /api/action?cmd=resumo` | envia o resumo diário agora (pede login) |
| `POST /api/action?cmd=limparEventos` | apaga a central de eventos (pede login) |
| `POST /api/action?cmd=reiniciar` | reinicia o ESP32 (pede login) |
