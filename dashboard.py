#!/usr/bin/env python3
"""
dashboard.py — painel interativo do Live Matching Engine (Streamlit + ZMQ + Plotly).

Assina o market-data feed da engine (SUB tcp://localhost:5556) e mostra ao vivo:
gráfico de preço, order book (profundidade) e o tape de trades. Para ter o book,
pede "print book" periodicamente pela porta de order-entry (PUSH tcp://localhost:5555)
e parseia a resposta — ou seja, NÃO precisa mexer no C++.

Além de VER, dá pra ENVIAR ordens (a barra lateral usa o mesmo PUSH):
  - Gerador AUTOMÁTICO: liga/desliga + slider de velocidade (rápido <-> lento);
  - Ordem MANUAL: formulário (limit/market) e um campo pra digitar o comando cru.

Os painéis separam "🧾 minhas ordens" (o que VOCÊ enviou) do "📉 tape do mercado"
(todos os trades — o feed não marca a origem de cada trade, então não dá pra dizer
"esse trade é meu"; o que é 100% rastreável é o comando que você mandou).

COMO RODAR (2 terminais — o dashboard já gera as ordens sozinho):
  1) ./mstly_engine.exe zmq          # a engine/gateway
  2) streamlit run dashboard.py      # este painel (abre no navegador)
  (market_feeder.py continua existindo como gerador de linha de comando, opcional.)
"""
import re
import time
from collections import deque

import plotly.graph_objects as go
import streamlit as st
import zmq

import market_sim

PULL_ENDPOINT = "tcp://localhost:5555"   # order entry (mandamos ordens + "print book")
PUB_ENDPOINT  = "tcp://localhost:5556"   # market data (ouvimos os eventos)

VERDE, VERMELHO = "#26a69a", "#ef5350"
MAX_PRECOS, MAX_TRADES, MAX_MINHAS = 400, 40, 30
BOOK_NIVEIS = 12                         # só os N níveis mais próximos do topo
TRADE_RE = re.compile(r"Trade, price: ([\d.]+), qty: (\d+)")

st.set_page_config(page_title="Live Matching Engine", layout="wide", page_icon="📈")


# --- conexão ZMQ mantida viva entre os reruns do Streamlit -------------------
@st.cache_resource
def conectar():
    ctx = zmq.Context.instance()
    sub = ctx.socket(zmq.SUB)
    sub.connect(PUB_ENDPOINT)
    sub.setsockopt_string(zmq.SUBSCRIBE, "")
    push = ctx.socket(zmq.PUSH)
    push.connect(PULL_ENDPOINT)
    time.sleep(0.3)  # handshake do SUB (slow joiner)
    return sub, push


def init_estado():
    ss = st.session_state
    ss.setdefault("precos", deque(maxlen=MAX_PRECOS))       # last price ao longo do tempo
    ss.setdefault("trades", deque(maxlen=MAX_TRADES))       # (hora, preco, qty) — mercado
    ss.setdefault("minhas_ordens", deque(maxlen=MAX_MINHAS))# (hora, comando) — o que EU enviei
    ss.setdefault("book", {"bids": {}, "asks": {}})         # preco -> qty agregada
    ss.setdefault("n_trades", 0)
    ss.setdefault("volume", 0)
    ss.setdefault("sim", market_sim.novo_estado())          # estado do gerador automático


def enviar(push, cmd):
    """Manda um comando pela porta de order-entry e registra em 'minhas ordens'."""
    push.send_string(cmd)
    st.session_state.minhas_ordens.appendleft((time.strftime("%H:%M:%S"), cmd))


def parse_lado(txt):
    """'198 @ 99.97' -> (198, 99.97); vazio/invalido -> None."""
    txt = txt.strip()
    if not txt or "@" not in txt:
        return None
    try:
        q, p = txt.split("@")
        return int(q.strip()), float(p.strip())
    except (ValueError, IndexError):
        return None


def drenar_feed(sub):
    """Lê tudo que a engine publicou desde o último tick e atualiza o estado."""
    ss = st.session_state
    book_tmp, coletando = None, False
    agora = time.strftime("%H:%M:%S")

    while True:
        try:
            msg = sub.recv_string(zmq.NOBLOCK).strip()
        except zmq.Again:
            break
        if not msg:
            continue

        market_sim.registrar_evento(ss.sim, msg)   # aprende ids p/ cancelamentos automáticos

        if msg.startswith("Ordens de Compra"):      # início de um snapshot de book
            book_tmp, coletando = {"bids": {}, "asks": {}}, True
            continue
        if coletando and "|" in msg:
            miolo = msg.replace("|", "").strip()
            if miolo and set(miolo) <= set("-"):    # linha separadora
                continue
            esq, _, dire = msg.partition("|")
            b, a = parse_lado(esq), parse_lado(dire)
            if b:
                book_tmp["bids"][b[1]] = book_tmp["bids"].get(b[1], 0) + b[0]
            if a:
                book_tmp["asks"][a[1]] = book_tmp["asks"].get(a[1], 0) + a[0]
            continue
        coletando = False

        m = TRADE_RE.match(msg)
        if m:
            preco, qty = float(m.group(1)), int(m.group(2))
            ss.trades.appendleft((agora, preco, qty))
            ss.precos.append(preco)
            ss.n_trades += 1
            ss.volume += qty

    if book_tmp is not None:
        ss.book = book_tmp


def fig_preco():
    ss = st.session_state
    fig = go.Figure()
    if ss.precos:
        fig.add_trace(go.Scatter(y=list(ss.precos), mode="lines",
                                 line=dict(color=VERDE, width=2)))
    fig.update_layout(height=360, margin=dict(l=10, r=10, t=30, b=10),
                      title="Preço (last trade)", xaxis_title="ticks de trade",
                      yaxis_title="preço", showlegend=False, uirevision="preco")
    return fig


def fig_book():
    ss = st.session_state
    bids, asks = ss.book["bids"], ss.book["asks"]
    bid_pr = sorted(bids, reverse=True)[:BOOK_NIVEIS]   # 12 melhores bids (maiores preços)
    ask_pr = sorted(asks)[:BOOK_NIVEIS]                 # 12 melhores asks (menores preços)
    ordem = sorted(set(bid_pr + ask_pr))               # eixo y SEMPRE em ordem numérica crescente
    fig = go.Figure()
    if bid_pr:
        fig.add_trace(go.Bar(y=[f"{p:.2f}" for p in bid_pr], x=[bids[p] for p in bid_pr],
                             orientation="h", marker_color=VERDE, name="bids"))
    if ask_pr:
        fig.add_trace(go.Bar(y=[f"{p:.2f}" for p in ask_pr], x=[asks[p] for p in ask_pr],
                             orientation="h", marker_color=VERMELHO, name="asks"))
    fig.update_layout(height=360, margin=dict(l=10, r=10, t=30, b=10),
                      title="Order book (profundidade) — bids embaixo, asks em cima",
                      xaxis_title="quantidade", yaxis_title="preço", barmode="overlay",
                      uirevision="book", legend=dict(orientation="h", y=1.1))
    # trava o eixo de preço em ordem numérica (senão o eixo categórico embaralha)
    fig.update_yaxes(categoryorder="array", categoryarray=[f"{p:.2f}" for p in ordem])
    return fig


def barra_lateral(push):
    """Controles: gerador automático (liga/velocidade) + envio manual de ordens."""
    sb = st.sidebar
    sb.header("🎮 Controles")

    sb.subheader("🤖 Gerador automático")
    sb.toggle("ligar", key="auto_on", value=False)
    sb.slider("velocidade (ordens/s)", 1, 50, 10, key="auto_rate")
    sb.toggle("só criar liquidez (você fecha os trades)", key="so_liquidez", value=True)
    sb.caption("Com 'só liquidez' ligado (padrão): o gerador só monta o book "
               "(bids/asks) e VOCÊ dispara os trades pelos botões. Desligue para "
               "um mercado que também fecha trades sozinho.")

    sb.divider()
    sb.subheader("✍️ Ordem manual")
    # 'tipo' fica FORA do form pra o campo 'preço' poder sumir quando for market
    # (market não tem preço — casa no melhor preço disponível do lado oposto).
    tipo = sb.selectbox("tipo", ["limit", "market"], key="tipo_ordem")
    with sb.form("ordem_manual"):
        side = st.selectbox("lado", ["buy", "sell"])
        preco = st.number_input("preço", value=100.00, step=0.01, format="%.2f") \
            if tipo == "limit" else None
        qty = st.number_input("qty", value=100, min_value=1, step=10)
        if st.form_submit_button("Enviar ordem", type="primary"):
            cmd = f"limit {side} {preco:.2f} {int(qty)}" if tipo == "limit" \
                  else f"market {side} {int(qty)}"
            enviar(push, cmd)
            sb.success(f"enviado: {cmd}")

    sb.divider()
    sb.subheader("⌨️ Comando cru")
    with sb.form("cmd_livre", clear_on_submit=True):
        cru = st.text_input("digite e Enter", placeholder="ex: cancel order 5")
        if st.form_submit_button("Enviar") and cru.strip():
            enviar(push, cru.strip())
            sb.success(f"enviado: {cru.strip()}")


@st.fragment(run_every=1.0)
def painel():
    sub, push = conectar()
    drenar_feed(sub)

    # gerador automático: gera ~rate ordens por tick (tick = 1s)
    ss = st.session_state

    bids, asks = ss.book["bids"], ss.book["asks"]
    best_bid = max(bids) if bids else None
    best_ask = min(asks) if asks else None

    if ss.get("auto_on", False):
        so_liq = ss.get("so_liquidez", True)
        market_sim.avancar_mid(ss.sim)                      # mid anda UMA vez por tick
        for _ in range(int(ss.get("auto_rate", 10))):
            cmd = market_sim.proxima_ordem(ss.sim, so_liquidez=so_liq,
                                           best_bid=best_bid, best_ask=best_ask,
                                           _avancar_mid=False)
            if cmd:
                push.send_string(cmd)

    push.send_string("print book")  # snapshot que chega no próximo tick

    last = ss.precos[-1] if ss.precos else None
    spread = (best_ask - best_bid) if (best_bid is not None and best_ask is not None) else None
    modo = f"🟢 auto {int(ss.get('auto_rate', 10))}/s" if ss.get("auto_on") else "⚪ manual"

    c1, c2, c3, c4 = st.columns(4)
    c1.metric("Last price", f"{last:.2f}" if last is not None else "—")
    c2.metric("Best bid / ask",
              f"{best_bid:.2f} / {best_ask:.2f}" if spread is not None else "—")
    c3.metric("Spread", f"{spread:.2f}" if spread is not None else "—", delta=modo,
              delta_color="off")
    c4.metric("Trades / volume", f"{ss.n_trades} / {ss.volume}")

    g1, g2 = st.columns(2)
    g1.plotly_chart(fig_preco(), use_container_width=True, key="g_preco")
    g2.plotly_chart(fig_book(), use_container_width=True, key="g_book")

    col_eu, col_mkt = st.columns([1, 2])
    with col_eu:
        st.subheader("🧾 Minhas ordens")
        if ss.minhas_ordens:
            st.dataframe(
                {"hora": [o[0] for o in ss.minhas_ordens],
                 "comando": [o[1] for o in ss.minhas_ordens]},
                use_container_width=True, height=240, hide_index=True, key="tape_eu")
        else:
            st.caption("Envie uma ordem na barra lateral → (aparece aqui).")
    with col_mkt:
        with st.expander("📉 Tape do mercado — todos os trades (clique p/ minimizar)",
                         expanded=True):
            if ss.trades:
                st.dataframe(
                    {"hora": [t[0] for t in ss.trades],
                     "preço": [f"{t[1]:.2f}" for t in ss.trades],
                     "qty": [t[2] for t in ss.trades]},
                    use_container_width=True, height=240, hide_index=True, key="tape_mkt")
            else:
                st.info("Aguardando trades... ligue o gerador automático ou envie uma ordem. (A engine `zmq` está rodando?)")


if __name__ == "__main__":
    st.title("📈 Live Matching Engine")
    st.caption("Feed ZMQ em tempo real · book + tape + preço · envie ordens pela barra lateral · núcleo em C++")
    init_estado()
    _, _push = conectar()
    barra_lateral(_push)
    painel()
