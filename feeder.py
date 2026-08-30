import zmq
import time
import sys

def main():
    ctx = zmq.Context()
    
    print("Conectando sockets...")
    
    # SUB para Market Data
    sub = ctx.socket(zmq.SUB)
    sub.connect("tcp://localhost:5556")
    sub.setsockopt_string(zmq.SUBSCRIBE, "")
    
    # PUSH para Order Entry
    push = ctx.socket(zmq.PUSH)
    push.connect("tcp://localhost:5555")
    
    # Evitar o slow joiner syndrome do SUB
    print("Aguardando handshake do SUB (slow joiner)...")
    time.sleep(1)
    
    ordens_teste = [
        "limit buy 10.00 100",
        "limit sell 11.00 50",
        "market buy 50",    # Agressor: vai consumir o limit sell
        "print book",
        "stop"
    ]
    
    for ordem in ordens_teste:
        print(f"\n[PUSH] Enviando comando: {ordem}")
        push.send_string(ordem)
        time.sleep(0.2) # Pausa leve pra dar tempo de processar e publicar
        
        # Esvazia a fila do PUB (market data e logs)
        try:
            while True:
                msg = sub.recv_string(zmq.NOBLOCK)
                print(f"[PUB] Market Data -> {msg.strip()}")
        except zmq.Again:
            pass

    # Flush final
    time.sleep(0.5)
    try:
        while True:
            msg = sub.recv_string(zmq.NOBLOCK)
            print(f"[PUB] Market Data -> {msg.strip()}")
    except zmq.Again:
        pass
        
    print("\nTeste concluído. O servidor (C++) deve ter recebido o stop, feito o dump e desligado.")

if __name__ == "__main__":
    main()
