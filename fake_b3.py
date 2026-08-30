#!/usr/bin/env python3
"""
fake_b3.py — simulador de mercado ("fake B3") para a matching engine.

Gera um fluxo CONTINUO e pseudo-realista de ordens e injeta na engine via ZMQ:
  - PUSH  tcp://localhost:5555  (order entry) -> manda os comandos
  - SUB   tcp://localhost:5556  (market data) -> escuta os eventos da engine

Diferente do feeder.py (que so' manda 5 ordens fixas e morre), este fica rodando
e faz o book "respirar". Modelo simples mas vivo:
  - um preco-justo (mid) que faz RANDOM WALK (simula pressao/noticia);
  - LIMITS nos dois lados em torno do mid (formam o spread);
  - MARKETS ocasionais que ATRAVESSAM o book e geram trades;
  - CANCELAMENTOS aleatorios das ordens que ESTE simulador criou (ele aprende os
    ids lendo os eventos "Order created" do proprio feed de market data).

PRE-REQUISITO: o gateway precisa estar no ar ->  ./mstly_engine.exe zmq

Uso:
  python fake_b3.py                      # 5 ordens/s, infinito (Ctrl+C p/ parar)
  python fake_b3.py --rate 20            # mercado mais agitado
  python fake_b3.py --duration 30        # roda 30s e para (nao derruba a engine)
  python fake_b3.py --seed 42            # reproduzivel
  python fake_b3.py --stop-on-exit       # ao sair, manda 'stop' e derruba a engine
"""
import argparse
import random
import time

import zmq

# --- parametros do mercado (mexa aqui pra mudar o "humor" do mercado) --------
MID_INICIAL      = 100.00   # preco-justo inicial
VOLATILIDADE     = 0.05     # desvio-padrao do passo do random walk do mid
SPREAD_TICKS     = 5        # ate quantos ticks (0.01) longe do mid um limit entra
QTY_MIN, QTY_MAX = 10, 200  # faixa de quantidade por ordem
PROB_MARKET      = 0.20     # fracao das ENTRADAS que sao market (agressoras)
PROB_CANCEL      = 0.25     # fracao das ACOES que sao cancelamento (se houver ordem viva)
MAX_VIVAS        = 1000     # teto da lista de ids rastreados (poda os mais antigos)
TICK             = 0.01     # granularidade de preco da engine (centavos)


def main():
    ap = argparse.ArgumentParser(description="Simulador de mercado (fake B3) para a matching engine.")
    ap.add_argument("--rate", type=float, default=5.0, help="ordens por segundo (default: 5)")
    ap.add_argument("--duration", type=float, default=0.0, help="segundos de execucao (0 = infinito)")
    ap.add_argument("--seed", type=int, default=None, help="semente do RNG (reprodutibilidade)")
    ap.add_argument("--stop-on-exit", action="store_true", help="manda 'stop' pra engine ao sair")
    args = ap.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    ctx = zmq.Context()
    push = ctx.socket(zmq.PUSH)
    push.connect("tcp://localhost:5555")
    sub = ctx.socket(zmq.SUB)
    sub.connect("tcp://localhost:5556")
    sub.setsockopt_string(zmq.SUBSCRIBE, "")

    print("fake B3 conectada (PUSH :5555 / SUB :5556). Aguardando handshake do SUB...", flush=True)
    time.sleep(1.0)  # evita o slow-joiner do PUB/SUB (senao os 1os eventos se perdem)

    mid = MID_INICIAL
    vivas = []          # ids de ordens que ESTE simulador criou (aproximacao: podem ja ter morrido)
    n_cmd = 0
    n_trades = 0
    t0 = time.time()
    intervalo = 1.0 / args.rate if args.rate > 0 else 0.0

    def drenar_feed():
        """Le tudo que a engine publicou desde a ultima vez (nao-bloqueante)."""
        nonlocal n_trades
        while True:
            try:
                msg = sub.recv_string(zmq.NOBLOCK).strip()
            except zmq.Again:
                break
            if not msg:
                continue
            if msg.startswith("Order created:"):
                pid = msg.split()[-1]        # o id e' sempre o ultimo token
                if pid.isdigit():
                    vivas.append(pid)
                    if len(vivas) > MAX_VIVAS:
                        del vivas[:len(vivas) - MAX_VIVAS]  # poda os mais antigos
            elif msg.startswith("Trade"):
                n_trades += 1
                print(f"        >> {msg}", flush=True)   # destaca a execucao

    try:
        while True:
            if args.duration > 0 and (time.time() - t0) >= args.duration:
                break

            # 1) random walk do preco-justo (nunca abaixo de 0.10)
            mid = max(0.10, mid + random.gauss(0, VOLATILIDADE))

            # 2) decide a acao deste tick
            if vivas and random.random() < PROB_CANCEL:
                pid = vivas.pop(random.randrange(len(vivas)))   # cancela uma viva aleatoria
                cmd = f"cancel order {pid}"
            elif random.random() < PROB_MARKET:
                side = random.choice(("buy", "sell"))           # market agressor
                cmd = f"market {side} {random.randint(QTY_MIN, QTY_MAX)}"
            else:
                side = random.choice(("buy", "sell"))           # limit passivo em torno do mid
                off = random.randint(1, SPREAD_TICKS) * TICK
                preco = mid - off if side == "buy" else mid + off
                cmd = f"limit {side} {preco:.2f} {random.randint(QTY_MIN, QTY_MAX)}"

            push.send_string(cmd)
            n_cmd += 1
            print(f"[{n_cmd:>5}] mid={mid:7.2f}  ->  {cmd}", flush=True)

            drenar_feed()
            if intervalo:
                time.sleep(intervalo)
    except KeyboardInterrupt:
        print("\n(interrompido pelo usuario)", flush=True)

    time.sleep(0.3)
    drenar_feed()  # colhe os ultimos eventos

    if args.stop_on_exit:
        push.send_string("stop")
        print("enviado 'stop' -> a engine encerra.", flush=True)

    print(f"\nResumo: {n_cmd} comandos enviados, {n_trades} trades observados.", flush=True)


if __name__ == "__main__":
    main()
