#include <iostream>
#include <string>

#include "../engine/include/CommandProcessor.h"
#include "../engine/include/Gateway.h"
#include "../engine/include/AsyncJournal.h"  // Fase B (assincrono, ping-pong + drain)

// ---------------------------------------------------------------------------
//  Ponto de entrada. Dois modos, MESMO parser (CommandProcessor) nos dois:
//    mstly_engine repl   -> le comandos do stdin, imprime eventos no stdout
//                           (reproduz os exemplos ">>> limit buy 10 100" do enunciado)
//    mstly_engine zmq    -> sobe o gateway ZMQ (PULL :5555 in / PUB :5556 out)
//  O default e' 'repl' (mais facil de testar e de avaliar).
// ---------------------------------------------------------------------------

// Modo terminal: cada evento vira uma linha no stdout, e tudo (feed + eventos)
// é jornalizado em disco (Fase B: assíncrono) pra consulta posterior no pandas.
static int run_repl() {
    AsyncJournal journal("journal.tsv");
    // O sink é o MESMO evento indo pra dois "renderers": o terminal e o journal.
    CommandProcessor cp([&journal](const std::string& s) {
        std::cout << s << "\n";
        journal.record("EV", s);
    });
    std::string linha;
    while (std::getline(std::cin, linha)) {
        if (!linha.empty()) journal.record("IN", linha);  // registra o feed
        cp.process_line(linha);
    }
    return 0;
}

// Modo ao vivo: order-entry gateway (PULL) + market-data feed (PUB).
static int run_zmq() {
    std::cout << "Iniciando Gateway ZMQ (PULL tcp://*:5555 / PUB tcp://*:5556)...\n";
    Gateway gateway("tcp://*:5555", "tcp://*:5556");
    gateway.run();  // loop bloqueante; encerra ao receber "stop"/"quit"
    return 0;
}

int main(int argc, char** argv) {
    const std::string modo = (argc > 1) ? argv[1] : "repl";
    if (modo == "repl") return run_repl();
    if (modo == "zmq")  return run_zmq();
    std::cerr << "Uso: " << argv[0] << " [repl|zmq]\n";
    return 1;
}
