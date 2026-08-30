#pragma once

#include <string>
#include <zmq.hpp>

#include "CommandProcessor.h"
#include "AsyncJournal.h"

// =============================================================================
//  Gateway — ADAPTADOR DE REDE (Fase 7). Vive FORA do nucleo.
//
//  Dono dos sockets ZMQ. Dois papeis classicos de uma exchange:
//    - PULL  (bind tcp://*:5555): ORDER-ENTRY GATEWAY. Recebe comandos-texto de
//      um ou varios clientes (feeders) que dao PUSH. PULL/PUSH faz o balanceamento
//      e enfileira de forma confiavel (o PUSH bloqueia/enfileira ate haver peer),
//      entao nao ha perda de comandos na entrada.
//    - PUB   (bind tcp://*:5556): MARKET-DATA FEED. Publica TODOS os eventos
//      (Order created/cancelled, Trade, snapshots de book) p/ qualquer numero de
//      assinantes (listeners) via SUB. Fan-out 1->N, desacoplado.
//
//  O truque de arquitetura: o sink do CommandProcessor e' um lambda que PUBLICA
//  no PUB. Assim NAO existe logica de matching aqui — a mesma engine/parser do
//  REPL roda, e cada linha de saida vira uma mensagem de market-data. Rede e
//  dominio ficam totalmente separados.
//
//  ATENCAO ao "slow joiner" do PUB/SUB (documentado no README/relatorio): um SUB
//  que conecta DEPOIS do PUB comecar a publicar PERDE as mensagens iniciais (o
//  handshake da assinatura leva alguns ms). Por isso: suba o listener ANTES do
//  feeder.
// =============================================================================
class Gateway {
public:
    Gateway(const std::string& pull_endpoint = "tcp://*:5555",
            const std::string& pub_endpoint  = "tcp://*:5556",
            const std::string& journal_path  = "journal.tsv");

    // Loop de servico: recebe 1 comando por mensagem no PULL, roda no
    // CommandProcessor (que publica cada evento no PUB via sink). Bloqueante.
    // Encerra ao receber a linha "quit" ou "stop".
    void run();

private:
    void publicar(const std::string& linha);  // sink: envia a linha pelo PUB

    zmq::context_t  ctx_;
    zmq::socket_t   pull_;    // entrada de comandos (order entry)
    zmq::socket_t   pub_;     // saida de eventos (market data)
    AsyncJournal    journal_; // Fase B: journaling assincrono (feed + eventos)
    CommandProcessor proc_;   // parser+engine; sink -> publicar()
};
