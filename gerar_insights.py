#!/usr/bin/env python3
# ============================================================
# INSIGHTS DA HORTA (roda no GitHub Actions, de manhã e à noite)
#
#   1. Pede ao ESP32, pelo Firebase, o histórico, os eventos e os reabastecimentos
#      (o mesmo mecanismo que o painel usa; o ESP32 nem percebe a diferença).
#   2. Busca clima e previsão no Open-Meteo; calcula fase da lua.
#   3. CALCULA em código as estatísticas e tendências (a IA não faz contas).
#   4. Manda os números para o Gemini, que só interpreta e escreve a análise.
#   5. Valida a resposta e grava em /horta/insights no Firebase (o painel lê de lá).
#
# Só usa a biblioteca padrão do Python (3.9+).
#
# Variáveis de ambiente (no GitHub: Settings › Secrets and variables › Actions):
#   INSIGHTS_EMAIL, INSIGHTS_SENHA   usuário do Firebase só para este trabalho
#   GEMINI_API_KEY                   chave do Google AI Studio
#   HORTA_LAT, HORTA_LON             coordenadas da horta (segredo: o repositório é público)
#   GEMINI_MODEL                     opcional (padrão: gemini-flash-latest)
#   TURNO                            opcional: manha | noite (padrão: pelo horário)
#
# Testes locais (sem Firebase nem ESP32):
#   python3 gerar_insights.py --fonte-local http://localhost:8080 --saida saida.json --ia-falsa
# ============================================================

import argparse
import json
import math
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import date, datetime, timedelta, timezone
from zoneinfo import ZoneInfo

TZ = ZoneInfo("America/Sao_Paulo")
PASTA = os.path.dirname(os.path.abspath(__file__))

# A chave da API web do Firebase não é segredo (já está no painel público)
FIREBASE_API_KEY = os.environ.get("FIREBASE_API_KEY", "AIzaSyDzuPqq1XixK6Gx5jLvfZBBN0zoWbhFFWQ")
FIREBASE_DB = os.environ.get("FIREBASE_DB_URL", "https://horta-inteligente-3bdaa-default-rtdb.firebaseio.com")
FIREBASE_AUTH = os.environ.get("FIREBASE_AUTH_URL", "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword")
GEMINI_BASE = os.environ.get("GEMINI_BASE_URL", "https://generativelanguage.googleapis.com/v1beta")
OPEN_METEO = os.environ.get("OPEN_METEO_URL", "https://api.open-meteo.com/v1/forecast")

MANTER_INSIGHTS = 28      # 14 dias de manhã e noite
ESPERA_ESP_S = 90         # tempo máximo para o ESP32 responder cada pedido
ESTADO_VELHO_S = 300      # estado publicado há mais que isso = horta provavelmente fora do ar


def log(*a):
    print(*a, flush=True)


# ============================================================
# HTTP
# ============================================================

class ErroHttp(Exception):
    def __init__(self, codigo, corpo, url=""):
        super().__init__("HTTP %s em %s: %s" % (codigo, url.split("?")[0], corpo[:300]))
        self.codigo = codigo
        self.corpo = corpo


def http(metodo, url, corpo=None, cabecalhos=None, timeout=30, tentativas=3):
    """Devolve (codigo, texto). Repete em falha de rede e em erro 5xx."""
    dados = None
    cab = dict(cabecalhos or {})
    if corpo is not None:
        dados = corpo if isinstance(corpo, bytes) else json.dumps(corpo, ensure_ascii=False).encode("utf-8")
        cab.setdefault("Content-Type", "application/json")
    ultimo = None
    for n in range(tentativas):
        try:
            req = urllib.request.Request(url, data=dados, method=metodo, headers=cab)
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            texto = e.read().decode("utf-8", "replace")
            if e.code < 500:
                return e.code, texto
            ultimo = ErroHttp(e.code, texto, url)
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            ultimo = e
        if n < tentativas - 1:
            time.sleep(3 * (n + 1))
    raise ultimo


def json_ou_none(texto):
    try:
        return json.loads(texto) if texto and texto.strip() else None
    except ValueError:
        return None


# ============================================================
# FIREBASE (Realtime Database pela API REST)
# ============================================================

class Firebase:
    def __init__(self, email, senha):
        codigo, txt = http("POST", FIREBASE_AUTH + "?key=" + FIREBASE_API_KEY,
                           {"email": email, "password": senha, "returnSecureToken": True})
        d = json_ou_none(txt) or {}
        if codigo != 200 or "idToken" not in d:
            motivo = (d.get("error") or {}).get("message", txt[:200])
            raise RuntimeError("Login no Firebase recusado (%s): %s" % (codigo, motivo))
        self.token = d["idToken"]

    def _url(self, caminho, **consulta):
        consulta["auth"] = self.token
        return FIREBASE_DB + caminho + ".json?" + urllib.parse.urlencode(consulta)

    def get(self, caminho, **consulta):
        codigo, txt = http("GET", self._url(caminho, **consulta))
        if codigo != 200:
            raise ErroHttp(codigo, txt, caminho)
        return json_ou_none(txt)

    def put(self, caminho, valor):
        codigo, txt = http("PUT", self._url(caminho, print="silent"), valor)
        if codigo not in (200, 204):
            raise ErroHttp(codigo, txt, caminho)

    def delete(self, caminho):
        codigo, txt = http("DELETE", self._url(caminho, print="silent"))
        if codigo not in (200, 204):
            raise ErroHttp(codigo, txt, caminho)


# ============================================================
# COLETA: ESP32 (pelo Firebase ou direto, nos testes)
# ============================================================

class FonteEsp:
    """Faz GET nas rotas /api/* do ESP32."""

    def get(self, url):
        raise NotImplementedError

    def estado(self):
        raise NotImplementedError


class FonteFirebase(FonteEsp):
    def __init__(self, fb):
        self.fb = fb

    def get(self, url):
        rid = "job-%d-%s" % (int(time.time()), os.urandom(3).hex())
        req = "/horta/paraEsp/req/" + rid
        self.fb.put(req, {"url": url, "metodo": "GET", "criado": {".sv": "timestamp"}})
        limite = time.time() + ESPERA_ESP_S
        while time.time() < limite:
            time.sleep(2)
            r = self.fb.get("/horta/doEsp/resp/" + rid)
            if not r:
                continue
            try:
                self.fb.delete("/horta/doEsp/resp/" + rid)
            except Exception:
                pass
            if r.get("codigo") != 200:
                log("  ESP32 respondeu %s para %s" % (r.get("codigo"), url))
                return None
            return json_ou_none(r.get("corpo"))
        try:
            self.fb.delete(req)   # desistiu: não deixa o pedido pendurado
        except Exception:
            pass
        log("  ESP32 não respondeu em %d s: %s" % (ESPERA_ESP_S, url))
        return None

    def estado(self):
        e = self.fb.get("/horta/doEsp/estado")
        if e and e.get("vistoEm"):
            e["_idadeSeg"] = max(0, time.time() - e["vistoEm"] / 1000.0)
        return e


class FonteLocal(FonteEsp):
    """Direto no simulador ou no ESP32, pela rede local (testes)."""

    def __init__(self, base):
        self.base = base.rstrip("/")

    def get(self, url):
        try:
            codigo, txt = http("GET", self.base + url, tentativas=2)
        except (urllib.error.URLError, OSError):
            return None
        return json_ou_none(txt) if codigo == 200 else None

    def estado(self):
        e = self.get("/api/data")
        if e:
            e["_idadeSeg"] = 0
        return e


# ============================================================
# ANÁLISE DOS SENSORES
# ============================================================

def media(v):
    v = [x for x in v if x is not None]
    return sum(v) / len(v) if v else None


def arred(x, n=1):
    return None if x is None else round(x, n)


def agrupar_por_dia(h):
    """Séries do /api/history agrupadas por dia local.
    Colunas: 0 solução, 1 ambiente, 2 vazão, 3 mín, 4 máx, 5 bomba %, 6 online %, 7 flags."""
    if not h or not h.get("ok"):
        return {}
    t0, passo = h["t0"], h["passo"]
    dias = {}
    for i, r in enumerate(h["d"]):
        if r is None:
            continue
        dt = datetime.fromtimestamp(t0 + i * passo, TZ)
        g = dias.setdefault(dt.date().isoformat(), {"sol": [], "amb": [], "flu": [], "bomba": [], "online": [], "horas": 0.0, "pico": None})
        g["horas"] += passo / 3600.0
        hora = dt.hour + dt.minute / 60.0
        if r[0] is not None:
            g["sol"].append(r[0])
            if g["pico"] is None or r[0] > g["pico"][1]:
                g["pico"] = (hora, r[0])
        if r[1] is not None:
            g["amb"].append(r[1])
        if r[2] is not None and r[2] > 0.3:
            g["flu"].append(r[2])
        if r[5] is not None:
            g["bomba"].append(r[5])
        if r[6] is not None:
            g["online"].append(r[6])
    resumo = {}
    for dia, g in dias.items():
        resumo[dia] = {
            "horasCobertas": round(g["horas"], 1),
            "solMed": arred(media(g["sol"]), 1), "solMin": arred(min(g["sol"]), 1) if g["sol"] else None,
            "solMax": arred(max(g["sol"]), 1) if g["sol"] else None,
            "horaPicoSol": arred(g["pico"][0], 1) if g["pico"] else None,
            "ambMed": arred(media(g["amb"]), 1), "ambMin": arred(min(g["amb"]), 1) if g["amb"] else None,
            "ambMax": arred(max(g["amb"]), 1) if g["amb"] else None,
            "fluMed": arred(media(g["flu"]), 2),
            "bombaPct": arred(media(g["bomba"]), 1), "onlinePct": arred(media(g["online"]), 1),
        }
    return resumo


def variacao_pct(novo, antigo):
    if novo is None or antigo is None or antigo == 0:
        return None
    return round((novo - antigo) / antigo * 100.0, 1)


def tendencias(dias, resumos_antigos, hoje):
    """Compara os últimos 3 dias completos com os 4 anteriores (7 dias) e, se houver
    resumos guardados, a última semana com a de 3 a 4 semanas atrás."""
    completos = sorted(d for d, g in dias.items() if d != hoje and g["horasCobertas"] >= 20)
    saida = {}
    if len(completos) >= 5:
        recentes, anteriores = completos[-3:], completos[:-3]
        for chave, nome in (("fluMed", "vazao_Lmin"), ("bombaPct", "bomba_ligada_pct"), ("solMed", "temp_solucao_C"), ("ambMed", "temp_ambiente_C")):
            r = media([dias[d][chave] for d in recentes])
            a = media([dias[d][chave] for d in anteriores])
            saida[nome] = {"ult3dias": arred(r, 2), "4dias_antes": arred(a, 2), "var_pct": variacao_pct(r, a)}
    if len(resumos_antigos) >= 14:
        ordem = sorted(resumos_antigos)
        sem_atual, sem_antiga = ordem[-7:], ordem[:7]
        saida["vazao_longo_prazo"] = {
            "ultima_semana": arred(media([resumos_antigos[d].get("fluMed") for d in sem_atual]), 2),
            "semana_mais_antiga_guardada": arred(media([resumos_antigos[d].get("fluMed") for d in sem_antiga]), 2),
            "dias_guardados": len(ordem),
        }
        saida["vazao_longo_prazo"]["var_pct"] = variacao_pct(saida["vazao_longo_prazo"]["ultima_semana"],
                                                            saida["vazao_longo_prazo"]["semana_mais_antiga_guardada"])
    return saida


def regressao(pares):
    """Mínimos quadrados y = a + b x. Devolve (a, b, r2) ou None."""
    n = len(pares)
    if n < 5:
        return None
    mx = sum(p[0] for p in pares) / n
    my = sum(p[1] for p in pares) / n
    sxx = sum((p[0] - mx) ** 2 for p in pares)
    if sxx < 4.0:     # temperaturas máximas quase iguais: não dá para aprender nada
        return None
    sxy = sum((p[0] - mx) * (p[1] - my) for p in pares)
    syy = sum((p[1] - my) ** 2 for p in pares)
    b = sxy / sxx
    a = my - b * mx
    r2 = (sxy * sxy) / (sxx * syy) if syy > 0 else 0.0
    return a, b, r2


def prever_solucao(dias, resumos_antigos, previsao_clima, hoje):
    """Estima a temperatura máxima da solução nos próximos dias a partir da relação
    histórica entre a máxima do ambiente e a máxima da solução."""
    pares = {}
    for d, g in dias.items():
        if d != hoje and g["horasCobertas"] >= 20 and g["ambMax"] is not None and g["solMax"] is not None:
            pares[d] = (g["ambMax"], g["solMax"])
    for d, g in resumos_antigos.items():
        if d not in pares and g.get("ambMax") is not None and g.get("solMax") is not None:
            pares[d] = (g["ambMax"], g["solMax"])
    reg = regressao(list(pares.values()))
    if not reg or reg[2] < 0.3:
        return None
    a, b, r2 = reg
    prev = []
    for dia in previsao_clima:
        if dia["data"] > hoje and dia.get("tmax") is not None:
            prev.append({"data": dia["data"], "ambMaxPrevisto": dia["tmax"], "solMaxEstimada": round(a + b * dia["tmax"], 1)})
    return {"pontos_usados": len(pares), "r2": round(r2, 2), "dias": prev[:4]}


def resumir_eventos(ev, agora):
    """Conta panes, reinícios e quedas de WiFi nas últimas 24 h e 7 dias."""
    if not ev:
        return None
    nomes = ev.get("panes") or []
    saida = {"panes_24h": {}, "panes_7d": {}, "panes_ativas_agora": [], "reinicios_7d": 0,
             "reinicios_inesperados_7d": 0, "quedas_wifi_7d": 0, "tempo_sem_wifi_min_7d": 0}
    inicio_aberto = {}
    for _seq, t, tipo, id_, dur, extra in sorted(ev.get("lista", []), key=lambda e: e[0]):
        idade = agora - t if t else None
        nome = nomes[id_] if tipo in (1, 2) and id_ < len(nomes) else str(id_)
        if tipo == 1:   # início de pane
            inicio_aberto[id_] = t
            if idade is not None and idade <= 7 * 86400:
                saida["panes_7d"][nome] = saida["panes_7d"].get(nome, 0) + 1
            if idade is not None and idade <= 86400:
                saida["panes_24h"][nome] = saida["panes_24h"].get(nome, 0) + 1
        elif tipo == 2:
            inicio_aberto.pop(id_, None)
        elif tipo == 3 and idade is not None and idade <= 7 * 86400:
            saida["reinicios_7d"] += max(1, dur)
            if extra in (4, 5, 6, 7, 9):    # travamento, watchdog (5 a 7) e queda de tensão (9)
                saida["reinicios_inesperados_7d"] += max(1, dur)
        elif tipo == 7 and idade is not None and idade <= 7 * 86400:
            saida["quedas_wifi_7d"] += 1
            saida["tempo_sem_wifi_min_7d"] += round(dur / 60.0)
    saida["panes_ativas_agora"] = [nomes[i] for i in inicio_aberto if i < len(nomes)]
    return saida


def analisar_reabastecimentos(solucoes, et0_diario, hoje, previsao_clima):
    """Intervalo entre reabastecimentos e relação com a evapotranspiração (ET0)."""
    if not solucoes or not solucoes.get("lista"):
        return None
    lista = solucoes["lista"]
    atual = lista[-1]
    agora = solucoes.get("agora") or int(time.time())
    saida = {"idade_solucao_dias": round((agora - atual["inicio"]) / 86400.0, 1),
             "ja_chegou_ao_minimo": bool(atual.get("minimo"))}
    ciclos = []
    for r in lista[:-1][-8:]:
        if r.get("minimo") and r["minimo"] - r["inicio"] >= 86400:
            ciclos.append(r)
    saida["ciclos_completos_usados"] = len(ciclos)
    if ciclos:
        dur = [(r["minimo"] - r["inicio"]) / 86400.0 for r in ciclos]
        saida["dias_ate_minimo_medio"] = round(sum(dur) / len(dur), 1)
        saida["dias_ate_minimo_min_max"] = [round(min(dur), 1), round(max(dur), 1)]

    def et0_entre(ini_ts, fim_ts):
        total, dias_cobertos, dias_total = 0.0, 0, 0
        d = datetime.fromtimestamp(ini_ts, TZ).date()
        fim = datetime.fromtimestamp(fim_ts, TZ).date()
        while d <= fim:
            dias_total += 1
            v = et0_diario.get(d.isoformat())
            if v is not None:
                total += v
                dias_cobertos += 1
            d += timedelta(days=1)
        return (total, dias_cobertos == dias_total)

    totais = []
    for r in ciclos:
        t, completo = et0_entre(r["inicio"], r["minimo"])
        if completo:
            totais.append(t)
    if len(totais) >= 3 and not saida["ja_chegou_ao_minimo"]:
        media_t = sum(totais) / len(totais)
        desvio = (sum((x - media_t) ** 2 for x in totais) / len(totais)) ** 0.5
        acumulado, _ = et0_entre(atual["inicio"], agora)
        previstos = {d["data"]: d.get("et0") for d in previsao_clima if d["data"] > hoje and d.get("et0") is not None}
        recentes = [v for k, v in sorted(et0_diario.items())[-14:] if v is not None]
        padrao = sum(recentes) / len(recentes) if recentes else 3.0
        falta, dias_a_frente = media_t - acumulado, 0
        d = datetime.fromtimestamp(agora, TZ).date()
        while falta > 0 and dias_a_frente < 21:
            d += timedelta(days=1)
            falta -= previstos.get(d.isoformat(), padrao)
            dias_a_frente += 1
        saida["et0_medio_por_ciclo_mm"] = round(media_t, 1)
        saida["et0_acumulado_solucao_atual_mm"] = round(acumulado, 1)
        saida["previsao_proximo_minimo"] = {
            "em_dias": dias_a_frente if falta <= 0 else None,
            "data": d.isoformat() if falta <= 0 else None,
            "observacao": None if falta <= 0 else "mais de 21 dias",
            "confianca": "media" if len(totais) >= 5 and desvio / media_t < 0.25 else "baixa",
            "metodo": "evapotranspiração acumulada comparada à média dos ciclos anteriores",
        }
    return saida


# ============================================================
# CLIMA E LUA
# ============================================================

def baixar_clima(lat, lon):
    params = {
        "latitude": lat, "longitude": lon, "timezone": "America/Sao_Paulo", "past_days": 92, "forecast_days": 5,
        "daily": ",".join(["weather_code", "temperature_2m_max", "temperature_2m_min", "apparent_temperature_max",
                           "sunrise", "sunset", "daylight_duration", "uv_index_max", "precipitation_sum",
                           "precipitation_probability_max", "wind_speed_10m_max", "shortwave_radiation_sum",
                           "et0_fao_evapotranspiration"]),
    }
    codigo, txt = http("GET", OPEN_METEO + "?" + urllib.parse.urlencode(params), tentativas=3)
    d = json_ou_none(txt)
    if codigo != 200 or not d or "daily" not in d:
        raise RuntimeError("Open-Meteo falhou (%s): %s" % (codigo, txt[:200]))
    dd = d["daily"]
    dias, et0 = [], {}
    for i, dia in enumerate(dd["time"]):
        et0[dia] = dd["et0_fao_evapotranspiration"][i]
        dias.append({
            "data": dia, "tmax": dd["temperature_2m_max"][i], "tmin": dd["temperature_2m_min"][i],
            "sensacaoMax": dd["apparent_temperature_max"][i], "chuva_mm": dd["precipitation_sum"][i],
            "prob_chuva": dd["precipitation_probability_max"][i], "uv": dd["uv_index_max"][i],
            "vento_kmh": dd["wind_speed_10m_max"][i], "radiacao_MJ": dd["shortwave_radiation_sum"][i],
            "et0": dd["et0_fao_evapotranspiration"][i], "codigo": dd["weather_code"][i],
            "nascer": (dd["sunrise"][i] or "")[-5:], "por": (dd["sunset"][i] or "")[-5:],
            "luz_h": arred((dd["daylight_duration"][i] or 0) / 3600.0, 1),
        })
    return dias, et0


SINODICO = 29.530588853
NOVA_REF = datetime(2000, 1, 6, 18, 14, tzinfo=timezone.utc)


def fase_da_lua(agora_utc):
    idade = ((agora_utc - NOVA_REF).total_seconds() / 86400.0) % SINODICO
    f = idade / SINODICO
    ilum = (1 - math.cos(2 * math.pi * f)) / 2
    nomes = [(0.03, "nova"), (0.22, "crescente"), (0.28, "quarto crescente"), (0.47, "crescente gibosa"),
             (0.53, "cheia"), (0.72, "minguante gibosa"), (0.78, "quarto minguante"), (0.97, "minguante"), (1.01, "nova")]
    nome = next(n for lim, n in nomes if f < lim)

    def dias_ate(alvo):
        return round((alvo * SINODICO - idade) % SINODICO, 1)
    return {"fase": nome, "iluminacao_pct": round(ilum * 100), "idade_dias": round(idade, 1),
            "dias_ate_cheia": dias_ate(0.5), "dias_ate_nova": dias_ate(0.0),
            "aviso": "A influência da lua em hidroponia indoor/controlada não tem base científica sólida."}


# ============================================================
# FATOS CALCULADOS (alertas determinísticos, a IA só os comenta)
# ============================================================

def fatos_calculados(estado, dias, previsao_sol, clima, hoje):
    fatos = []
    if estado and estado.get("tSol") is not None:
        t = estado["tSol"]
        if t >= 28:
            fatos.append({"sev": "alerta", "fato": "Solução a %.1f °C agora (acima de 28 °C: risco alto de falta de oxigênio e podridão de raiz)." % t})
        elif t >= 26:
            fatos.append({"sev": "atencao", "fato": "Solução a %.1f °C agora (acima de 26 °C)." % t})
        elif t <= 15:
            fatos.append({"sev": "atencao", "fato": "Solução a %.1f °C agora (frio: manjericão e stévia sofrem abaixo de ~15 °C)." % t})
    if previsao_sol:
        for d in previsao_sol["dias"]:
            if d["solMaxEstimada"] >= 26:
                fatos.append({"sev": "atencao", "fato": "Em %s a solução deve chegar a ~%.1f °C (ambiente previsto %.0f °C), segundo a relação observada nos últimos dias." % (
                    d["data"], d["solMaxEstimada"], d["ambMaxPrevisto"])})
                break
    for d in clima:
        if d["data"] >= hoje and d.get("tmax") is not None and d["tmax"] >= 34:
            fatos.append({"sev": "atencao", "fato": "Previsão de %.0f °C no ar em %s (calor forte; alface e rúcula podem pendoar)." % (d["tmax"], d["data"])})
            break
    for d in clima:
        if d["data"] >= hoje and d.get("tmin") is not None and d["tmin"] <= 8:
            fatos.append({"sev": "atencao", "fato": "Previsão de mínima de %.0f °C em %s (frio: proteger manjericão e stévia)." % (d["tmin"], d["data"])})
            break
    if estado and estado.get("nivelBaixo"):
        fatos.append({"sev": "alerta", "fato": "O reservatório está no nível mínimo agora."})
    if estado and estado.get("panes"):
        fatos.append({"sev": "alerta", "fato": "Panes ativas agora: " + "; ".join(estado["panes"]) + "."})
    return fatos


# ============================================================
# GEMINI
# ============================================================

CATEGORIAS = ["temperatura", "vazao", "agua", "clima", "bomba", "culturas", "sistema", "curiosidade"]

PROMPT_SISTEMA = """Você é o analista da "Horta Inteligente": cruza os dados dos sensores de uma horta hidropônica com clima e previsão do tempo e escreve insights curtos e úteis para o dono da horta, em português do Brasil.

REGRAS
- Use SOMENTE os números do JSON recebido. Nunca invente valores, datas ou medições. Se um dado for null, ausente ou indisponível, não comente.
- Os números já foram calculados em código. Interprete e explique; não refaça contas.
- Cite os números que sustentam cada insight (no campo "dados").
- Seja específico para as culturas do perfil (alface, rúcula, hortelã, salsinha, cebolinha, manjericão, manjerona, orégano, stévia).
- Sem sensor de pH/EC: não afirme nada sobre pH/EC.
- Se os sensores da horta estiverem sem comunicação ("esp32.disponivel" falso), diga isso com destaque e analise só o que for possível (clima e previsão).
- A fase da lua tem pouca base científica em hidroponia controlada: no máximo UM insight de categoria "curiosidade", com confiança "baixa", e só se não houver nada mais útil. Nunca use a lua para recomendar ações.
- Prefira poucos insights bons a muitos fracos. Se nada relevante mudou, diga isso no resumo e traga 1 ou 2 insights.
- Não repita insights já dados ("insights_recentes"), a menos que a situação tenha mudado; nesse caso, diga o que mudou.
- Severidade: "alerta" = agir hoje; "atencao" = vale acompanhar ou se preparar; "info" = informação ou boa notícia.
- Confiança: "alta" (dado direto e claro), "media" (tendência com poucos dados), "baixa" (estimativa ou correlação fraca).

SAÍDA: responda SOMENTE com um objeto JSON, sem texto fora dele, neste formato:
{
  "resumo": "2 a 3 frases com a situação geral e o principal ponto do dia",
  "humor": "bom" | "atencao" | "alerta",
  "insights": [ {"categoria": "temperatura|vazao|agua|clima|bomba|culturas|sistema|curiosidade", "titulo": "até 70 caracteres", "texto": "1 a 3 frases, com a ação sugerida quando houver", "severidade": "info|atencao|alerta", "confianca": "alta|media|baixa", "dados": "números usados"} ],
  "tendencias": [ {"metrica": "nome", "direcao": "subindo|estavel|caindo", "texto": "uma frase"} ],
  "acoes": ["até 4 ações práticas, na ordem de prioridade"]
}
Limites: no máximo 6 insights, 4 tendências e 4 ações."""

FOCO = {
    "manha": "TURNO: MANHÃ. Foque no que esperar do dia (calor, chuva, evapotranspiração, risco para a solução) e no que fazer antes de o calor chegar. Faça um balanço curto da noite.",
    "noite": "TURNO: NOITE. Faça o balanço do dia (como a solução, a vazão e a bomba se comportaram) e prepare o dia seguinte. Destaque tendências da semana.",
}


def chamar_gemini(chave, modelo, sistema, usuario):
    url = "%s/models/%s:generateContent" % (GEMINI_BASE, modelo)
    corpo = {
        "systemInstruction": {"parts": [{"text": sistema}]},
        "contents": [{"role": "user", "parts": [{"text": usuario}]}],
        "generationConfig": {"temperature": 0.4, "responseMimeType": "application/json", "maxOutputTokens": 8192},
    }
    espera = [20, 60, 120]
    for n in range(len(espera) + 1):
        codigo, txt = http("POST", url, corpo, {"x-goog-api-key": chave}, timeout=120, tentativas=1)
        if codigo == 200:
            d = json_ou_none(txt) or {}
            uso = d.get("usageMetadata", {})
            log("  tokens: entrada %s, saída %s, raciocínio %s" % (uso.get("promptTokenCount"), uso.get("candidatesTokenCount"), uso.get("thoughtsTokenCount", 0)))
            cands = d.get("candidates") or []
            if not cands:
                raise RuntimeError("Gemini não devolveu candidatos: %s" % txt[:300])
            partes = (cands[0].get("content") or {}).get("parts") or []
            texto = "".join(p.get("text", "") for p in partes if not p.get("thought"))
            if not texto.strip():
                raise RuntimeError("Gemini devolveu resposta vazia (motivo: %s)" % cands[0].get("finishReason"))
            return texto
        if codigo == 404:
            modelos = listar_modelos(chave)
            raise RuntimeError("Modelo '%s' não encontrado. Defina GEMINI_MODEL com um destes: %s" % (modelo, ", ".join(modelos) or "(não consegui listar)"))
        if codigo in (429, 500, 503) and n < len(espera):
            log("  Gemini %s; nova tentativa em %d s" % (codigo, espera[n]))
            time.sleep(espera[n])
            continue
        raise RuntimeError("Gemini falhou (%s): %s" % (codigo, txt[:400]))


def listar_modelos(chave):
    try:
        codigo, txt = http("GET", GEMINI_BASE + "/models?pageSize=100", cabecalhos={"x-goog-api-key": chave}, tentativas=1)
        d = json_ou_none(txt) or {}
        return sorted(m["name"].split("/")[-1] for m in d.get("models", []) if "flash" in m.get("name", "") and "generateContent" in m.get("supportedGenerationMethods", []))
    except Exception:
        return []


def cortar(txt, n):
    txt = " ".join(str(txt or "").split())
    return txt if len(txt) <= n else txt[:n - 1].rstrip() + "…"


def validar_saida(bruto):
    """Aceita só o que tem o formato esperado; descarta o resto em vez de falhar."""
    txt = bruto.strip()
    if txt.startswith("```"):
        txt = txt.strip("`")
        txt = txt[4:] if txt.lower().startswith("json") else txt
    try:
        d = json.loads(txt)
    except ValueError as e:
        raise RuntimeError("A IA não devolveu JSON válido: %s | início: %s" % (e, txt[:200]))
    if not isinstance(d, dict):
        raise RuntimeError("A IA devolveu um JSON que não é um objeto.")
    sevs, confs, dirs = ("info", "atencao", "alerta"), ("alta", "media", "baixa"), ("subindo", "estavel", "caindo")
    insights = []
    for i in d.get("insights") or []:
        if not isinstance(i, dict) or not i.get("titulo") or not i.get("texto"):
            continue
        cat = i.get("categoria") if i.get("categoria") in CATEGORIAS else "sistema"
        conf = i.get("confianca") if i.get("confianca") in confs else "media"
        if cat == "curiosidade":
            conf = "baixa"
        insights.append({"categoria": cat, "titulo": cortar(i["titulo"], 80), "texto": cortar(i["texto"], 420),
                         "severidade": i.get("severidade") if i.get("severidade") in sevs else "info",
                         "confianca": conf, "dados": cortar(i.get("dados"), 200)})
    if not insights:
        raise RuntimeError("A IA não devolveu nenhum insight válido.")
    tend = [{"metrica": cortar(t.get("metrica"), 40), "direcao": t["direcao"], "texto": cortar(t.get("texto"), 200)}
            for t in (d.get("tendencias") or []) if isinstance(t, dict) and t.get("direcao") in dirs and t.get("texto")]
    humor = d.get("humor") if d.get("humor") in ("bom", "atencao", "alerta") else (
        "alerta" if any(i["severidade"] == "alerta" for i in insights) else "atencao" if any(i["severidade"] == "atencao" for i in insights) else "bom")
    return {"resumo": cortar(d.get("resumo"), 500), "humor": humor, "insights": insights[:6], "tendencias": tend[:4],
            "acoes": [cortar(a, 180) for a in (d.get("acoes") or []) if a][:4]}


def ia_falsa(dados):
    """Resposta de mentira para testar o resto do fluxo sem gastar chamadas."""
    return json.dumps({
        "resumo": "Resposta de teste (sem IA): os números foram calculados e o fluxo completo funcionou.",
        "humor": "atencao",
        "insights": [
            {"categoria": "temperatura", "titulo": "Solução deve esquentar nos próximos dias", "texto": "Texto de teste. A previsão indica calor; confira a sombra e o nível do reservatório.", "severidade": "atencao", "confianca": "media", "dados": "teste"},
            {"categoria": "vazao", "titulo": "Vazão estável na semana", "texto": "Texto de teste. Nenhuma queda de vazão relevante nos últimos dias.", "severidade": "info", "confianca": "alta", "dados": "teste"},
            {"categoria": "curiosidade", "titulo": "Lua %s" % dados["lua"]["fase"], "texto": "Texto de teste, só curiosidade.", "severidade": "info", "confianca": "baixa", "dados": "teste"},
        ],
        "tendencias": [{"metrica": "vazão", "direcao": "estavel", "texto": "Sem variação relevante."}],
        "acoes": ["Ação de teste 1", "Ação de teste 2"],
    }, ensure_ascii=False)


# ============================================================
# EXECUÇÃO
# ============================================================

def montar_dados(fonte, agora, lat, lon, resumos_antigos, insights_recentes, turno):
    hoje = agora.date().isoformat()
    log("Coletando dados da horta…")
    estado = fonte.estado()
    esp_ok = bool(estado) and estado.get("_idadeSeg", 0) < ESTADO_VELHO_S
    h7 = h24 = solucoes = eventos = None
    if esp_ok:
        h7 = fonte.get("/api/history?min=10080&pts=336")
        h24 = fonte.get("/api/history?min=1440&pts=96")
        solucoes = fonte.get("/api/solucoes")
        eventos = fonte.get("/api/eventos?n=150")
    else:
        log("  Horta sem comunicação recente (estado %s)." % ("ausente" if not estado else "com %d s" % estado.get("_idadeSeg", 0)))

    log("Buscando clima…")
    clima, et0 = baixar_clima(lat, lon)
    dias = agrupar_por_dia(h7)
    previsao_sol = prever_solucao(dias, resumos_antigos, clima, hoje)

    esp = {"disponivel": esp_ok, "ultima_noticia_min": None if not estado else round(estado.get("_idadeSeg", 0) / 60.0)}
    if estado and esp_ok:
        esp["agora"] = {k: estado.get(k) for k in ("tSol", "tAmb", "fluxo", "fluxoPresente", "tomadaStatus", "tomadaSeg", "nivelBaixo",
                                                  "modoManutencao", "panes")}
        esp["agora"]["wifi_rssi"] = (estado.get("diag") or {}).get("rssi")
        esp["agora"]["ligado_ha_dias"] = arred((estado.get("uptimeSeg") or 0) / 86400.0, 1)
        c = estado.get("cfg") or {}
        esp["ajustes"] = {"bomba_ligada_min": c.get("minOn"), "bomba_desligada_max_min": c.get("maxOff"), "vazao_minima_Lmin": c.get("vazaoMin")}
    if h24 and h24.get("ok"):
        esp["ultimas_24h"] = h24["stats"]
    if h7 and h7.get("ok"):
        esp["ultimos_7dias"] = h7["stats"]
    esp["por_dia"] = dias

    dados = {
        "agora_local": agora.strftime("%Y-%m-%d %H:%M"), "dia_da_semana": ["segunda", "terça", "quarta", "quinta", "sexta", "sábado", "domingo"][agora.weekday()],
        "turno": turno, "esp32": esp,
        "tendencias_calculadas": tendencias(dias, resumos_antigos, hoje),
        "estimativa_temp_solucao": previsao_sol,
        "eventos": resumir_eventos(eventos, int(agora.timestamp())),
        "reabastecimentos": analisar_reabastecimentos(solucoes, et0, hoje, clima),
        "clima": {"ultimos_dias_e_proximos": [d for d in clima if (date.fromisoformat(d["data"]) - agora.date()).days >= -3]},
        "lua": fase_da_lua(agora.astimezone(timezone.utc)),
        "insights_recentes": insights_recentes,
    }
    dados["fatos_calculados"] = fatos_calculados(estado if esp_ok else None, dias, previsao_sol, clima, hoje)
    if not esp_ok:
        quando = "sem nenhum registro" if not estado else "há %d min" % round(estado.get("_idadeSeg", 0) / 60.0)
        dados["fatos_calculados"].insert(0, {"sev": "alerta", "fato": "A horta está sem comunicação (última notícia %s). Os dados dos sensores não estão disponíveis." % quando})
    contexto = {"lua": dados["lua"], "clima": [{k: d[k] for k in ("data", "tmax", "tmin", "chuva_mm", "prob_chuva", "et0")} for d in clima if d["data"] >= hoje][:4]}
    return dados, contexto, esp_ok, h24


def resumo_do_dia(h24, hoje):
    """Linha compacta guardada no Firebase para tendências de longo prazo."""
    if not h24 or not h24.get("ok"):
        return None
    s = h24["stats"]
    return {"data": hoje, "bombaPct": s.get("bombaPct"), "ciclos": s.get("ciclos"), "cicloMedSeg": s.get("cicloMed"),
            "fluMed": s.get("fluMed"), "fluMin": s.get("fluMin"), "fluMax": s.get("fluMax"),
            "solMed": s.get("solMed"), "solMin": s.get("solMin"), "solMax": s.get("solMax"),
            "ambMed": s.get("ambMed"), "ambMin": s.get("ambMin"), "ambMax": s.get("ambMax"),
            "nivelBaixoMin": s.get("nivelBaixoMin"), "onlinePct": s.get("onlinePct")}


def main():
    ap = argparse.ArgumentParser(description="Gera os insights da horta com IA.")
    ap.add_argument("--fonte-local", help="URL do simulador/ESP32 na rede local (testes, sem Firebase)")
    ap.add_argument("--saida", help="grava o resultado neste arquivo JSON em vez do Firebase")
    ap.add_argument("--ia-falsa", action="store_true", help="não chama o Gemini (testes)")
    ap.add_argument("--mostrar-dados", action="store_true", help="imprime os dados enviados à IA")
    ap.add_argument("--turno", choices=["manha", "noite"], default=os.environ.get("TURNO") or None)
    args = ap.parse_args()

    agora = datetime.now(TZ)
    turno = args.turno or ("manha" if agora.hour < 15 else "noite")
    hoje = agora.date().isoformat()
    log("Insights da horta · %s · turno %s" % (agora.strftime("%d/%m/%Y %H:%M"), turno))

    lat, lon = os.environ.get("HORTA_LAT"), os.environ.get("HORTA_LON")
    if not lat or not lon:
        sys.exit("Defina HORTA_LAT e HORTA_LON (nos Secrets do GitHub).")

    fb = None
    resumos, recentes = {}, []
    if args.fonte_local:
        fonte = FonteLocal(args.fonte_local)
    else:
        email, senha = os.environ.get("INSIGHTS_EMAIL"), os.environ.get("INSIGHTS_SENHA")
        if not email or not senha:
            sys.exit("Defina INSIGHTS_EMAIL e INSIGHTS_SENHA (nos Secrets do GitHub).")
        fb = Firebase(email, senha)
        fonte = FonteFirebase(fb)
        resumos = fb.get("/horta/resumos", orderBy='"$key"', limitToLast=45) or {}
        hist = fb.get("/horta/insights/historico", orderBy='"$key"', limitToLast=6) or {}
        recentes = [{"quando": k, "titulos": [i["titulo"] for i in (v.get("insights") or [])]} for k, v in sorted(hist.items())]

    dados, contexto, esp_ok, h24 = montar_dados(fonte, agora, lat, lon, resumos, recentes, turno)
    if args.mostrar_dados:
        print(json.dumps(dados, ensure_ascii=False, indent=1))

    perfil = open(os.path.join(PASTA, "perfil_horta.md"), encoding="utf-8").read()
    sistema = PROMPT_SISTEMA + "\n\n" + FOCO[turno] + "\n\nPERFIL DA HORTA (escrito pelo dono):\n" + perfil
    usuario = "Dados de hoje (JSON):\n" + json.dumps(dados, ensure_ascii=False, separators=(",", ":"))
    log("Tamanho dos dados enviados: %.1f KB" % (len(usuario) / 1024.0))

    modelo = os.environ.get("GEMINI_MODEL") or "gemini-flash-latest"
    if args.ia_falsa:
        resultado, modelo = validar_saida(ia_falsa(dados)), "teste"
    else:
        chave = os.environ.get("GEMINI_API_KEY")
        if not chave:
            sys.exit("Defina GEMINI_API_KEY (nos Secrets do GitHub).")
        log("Pedindo a análise ao Gemini (%s)…" % modelo)
        resultado = None
        for tentativa in (1, 2):
            try:
                resultado = validar_saida(chamar_gemini(chave, modelo, sistema, usuario))
                break
            except RuntimeError as e:
                if tentativa == 2 or "JSON válido" not in str(e) and "nenhum insight" not in str(e):
                    raise
                log("  Resposta inutilizável (%s); tentando mais uma vez." % str(e)[:120])

    saida = dict(resultado)
    saida.update({"dataLocal": hoje, "hora": agora.strftime("%H:%M"), "turno": turno, "modelo": modelo,
                  "geradoEmLocal": agora.isoformat(timespec="minutes"), "esp32Disponivel": esp_ok, "contexto": contexto})
    previsao = (dados.get("reabastecimentos") or {}).get("previsao_proximo_minimo")
    if previsao:
        saida["previsaoReabastecimento"] = previsao

    if args.saida:
        with open(args.saida, "w", encoding="utf-8") as f:
            json.dump(saida, f, ensure_ascii=False, indent=1)
        log("Resultado salvo em %s" % args.saida)
        return

    saida["geradoEm"] = {".sv": "timestamp"}
    chave_hist = "%s_%s" % (hoje, turno)
    fb.put("/horta/insights/ultimo", saida)
    fb.put("/horta/insights/historico/" + chave_hist, saida)
    chaves = sorted((fb.get("/horta/insights/historico", shallow="true") or {}).keys())
    for velha in chaves[:-MANTER_INSIGHTS]:
        fb.delete("/horta/insights/historico/" + velha)
    if turno == "noite" and esp_ok:
        linha = resumo_do_dia(h24, hoje)
        if linha:
            fb.put("/horta/resumos/" + hoje, linha)
            ordem = sorted((fb.get("/horta/resumos", shallow="true") or {}).keys())
            for velha in ordem[:-120]:
                fb.delete("/horta/resumos/" + velha)
    log("Insights publicados em /horta/insights/ultimo (%s)." % chave_hist)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:      # o GitHub Actions marca a execução como falha e avisa por e-mail
        log("ERRO: %s" % e)
        sys.exit(1)
