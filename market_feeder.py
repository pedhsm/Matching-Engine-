#!/usr/bin/env python3
"""
market_feeder.py — gerador de mercado (feeder) em linha de comando.

Injeta um fluxo CONTINUO de ordens na engine via ZMQ (PUSH :5555) e escuta o
market data (SUB :5556). A LOGICA de mercado vive em market_sim.py (compartilhada
com o dashboard). Diferente do feeder.py (5 ordens fixas), este faz o book respirar.

E' a versao CLI do gerador que o dashboard (Live Matching Engine) tambem embute.

PRE-REQUISITO: o gateway no ar ->  ./mstly_engine.exe zmq

Uso:
  python market_feeder.py                 # 5 ordens/s, infinito (Ctrl+C p/ parar)
  python market_feeder.py --rate 20       # mercado mais agitado
  python market_feeder.py --duration 30   # roda 30s e para (nao derruba a engine)
  python market_feeder.py --seed 42       # reproduzivel
  python market_feeder.py --stop-on-exit  # ao sair, manda 'stop' e derruba a engine
"""
import argparse
import random
import time

import zmq

import market_sim


def main():
    ap = argparse.ArgumentParser(description="Gerador de mercado (feeder) para a matching engine.")
    ap.add_argument("--rate", type=float, default=5.0, help="ordens por segundo (default: 5)")
    ap.add_argument("--duration", type=float, default=0.0, help="segundos de execucao (0 = infinito)")
    ap.add_argument("--seed", type=int, default=None, help="semente do RNG (reprodutibilidade)")
    ap.add_argument("--stop-on-exit", action="store_true", help="manda 'stop' pra engine ao sair")
    ap.add_argument("--only-liquidity", action="store_true",
                    help="só posta liquidez (limits/cancels), não manda market")
    args = ap.parse_args()

    if args.seed is not None:
        random.seed(args.seed)

    ctx = zmq.Context()
    push = ctx.socket(zmq.PUSH)
    push.connect("tcp://localhost:5555")
    sub = ctx.socket(zmq.SUB)
    sub.connect("tcp://localhost:5556")
    sub.setsockopt_string(zmq.SUBSCRIBE, "")

    print("market feeder conectado (PUSH :5555 / SUB :5556). Aguardando handshake do SUB...", flush=True)
    time.sleep(1.0)  # evita o slow-joiner do PUB/SUB

    estado = market_sim.novo_estado()
    n_cmd = n_trades = 0
    t0 = time.time()
    intervalo = 1.0 / args.rate if args.rate > 0 else 0.0

    def drenar_feed():
        nonlocal n_trades
        while True:
            try:
                msg = sub.recv_string(zmq.NOBLOCK).strip()
            except zmq.Again:
                break
            if not msg:
                continue
            market_sim.registrar_evento(estado, msg)   # aprende ids p/ cancelar
            if msg.startswith("Trade"):
                n_trades += 1
                print(f"        >> {msg}", flush=True)

    try:
        while True:
            if args.duration > 0 and (time.time() - t0) >= args.duration:
                break
            cmd = market_sim.proxima_ordem(estado, so_liquidez=args.only_liquidity)
            push.send_string(cmd)
            n_cmd += 1
            print(f"[{n_cmd:>5}] mid={estado['mid']:7.2f}  ->  {cmd}", flush=True)
            drenar_feed()
            if intervalo:
                time.sleep(intervalo)
    except KeyboardInterrupt:
        print("\n(interrompido pelo usuario)", flush=True)

    time.sleep(0.3)
    drenar_feed()
    if args.stop_on_exit:
        push.send_string("stop")
        print("enviado 'stop' -> a engine encerra.", flush=True)
    print(f"\nResumo: {n_cmd} comandos enviados, {n_trades} trades observados.", flush=True)


if __name__ == "__main__":
    main()
