#!/usr/bin/env python3
"""
market_sim.py — o "cerebro" do simulador de mercado, compartilhado.

Isola a LOGICA de geracao de ordens (mid com random walk, limits nos dois lados,
markets agressores, cancelamentos) num unico lugar, pra o market_feeder.py (CLI) e
o dashboard.py (Live Matching Engine) usarem a MESMA regra sem duplicar codigo.

O estado (mid atual + ids de ordens vivas) e' um dict simples, pra o chamador
guardar onde quiser (variavel local na CLI, st.session_state no dashboard).
"""
import random

# --- parametros do mercado (mexa aqui pra mudar o "humor") -------------------
MID_INICIAL      = 100.00   # preco-justo inicial
VOLATILIDADE     = 0.05     # desvio-padrao do passo do random walk do mid
SPREAD_TICKS     = 5        # ate quantos ticks (0.01) longe do mid um limit entra
QTY_MIN, QTY_MAX = 10, 200  # faixa de quantidade por ordem
PROB_MARKET      = 0.20     # fracao das ENTRADAS que sao market (agressoras)
PROB_CANCEL      = 0.25     # fracao das ACOES que sao cancelamento (se houver ordem viva)
MAX_VIVAS        = 60       # teto de ordens vivas rastreadas — acima disso, cancela as + velhas
TICK             = 0.01     # granularidade de preco da engine


def novo_estado(mid=MID_INICIAL):
    """Cria o estado do simulador: preco-justo atual + ids de ordens vivas."""
    return {"mid": mid, "vivas": []}


def avancar_mid(estado):
    """Avanca o mid (random walk) uma vez. Chamar UMA vez por tick, nao por ordem."""
    estado["mid"] = max(0.10, estado["mid"] + random.gauss(0, VOLATILIDADE))


def registrar_evento(estado, msg):
    """Alimenta a lista de 'vivas' a partir de um evento do feed. So' olha os
    'Order created: ... <id>' (o id e' o ultimo token), pra depois poder cancelar."""
    if msg.startswith("Order created:"):
        pid = msg.split()[-1]
        if pid.isdigit():
            estado["vivas"].append(pid)


def proxima_ordem(estado, so_liquidez=False, best_bid=None, best_ask=None, _avancar_mid=True):
    """Devolve o PROXIMO comando-texto a enviar.

    so_liquidez=True: o gerador atua como MARKET MAKER — so' posta liquidez (limits
    nos dois lados) e cancelamentos, NUNCA manda market. Assim ele apenas monta o
    book (bids/asks) e quem FECHA os trades e' o usuario, pelos botoes.

    Se best_bid/best_ask forem informados (o dashboard conhece o topo do book), em
    modo so_liquidez as limits sao ANCORADAS p/ NUNCA cruzar (buy < best_ask, sell >
    best_bid) — garantindo ZERO trades vindos do gerador.

    _avancar_mid: se False, NAO faz random walk do mid (o chamador ja' fez via
    avancar_mid()). Isso evita que N chamadas num mesmo tick facam o mid divergir
    e gerar ordens que se cruzam entre si."""
    if _avancar_mid:
        estado["mid"] = max(0.10, estado["mid"] + random.gauss(0, VOLATILIDADE))
    mid, vivas = estado["mid"], estado["vivas"]

    # Janela deslizante: se tem ordens demais, cancela a MAIS VELHA primeiro.
    if len(vivas) > MAX_VIVAS:
        return f"cancel order {vivas.pop(0)}"

    if vivas and random.random() < PROB_CANCEL:
        pid = vivas.pop(random.randrange(len(vivas)))
        return f"cancel order {pid}"

    if not so_liquidez and random.random() < PROB_MARKET:
        side = random.choice(("buy", "sell"))
        return f"market {side} {random.randint(QTY_MIN, QTY_MAX)}"

    side = random.choice(("buy", "sell"))
    off = random.randint(1, SPREAD_TICKS) * TICK
    preco = mid - off if side == "buy" else mid + off

    # Em modo so_liquidez, ancora o preco p/ NUNCA cruzar o book
    if so_liquidez:
        if side == "buy" and best_ask is not None:
            preco = min(preco, best_ask - TICK)
        elif side == "sell" and best_bid is not None:
            preco = max(preco, best_bid + TICK)

    preco = max(TICK, preco)
    return f"limit {side} {preco:.2f} {random.randint(QTY_MIN, QTY_MAX)}"
