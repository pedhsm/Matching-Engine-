#pragma once

#include <string>
#include <cstdint>

#include "MatchingEngine.h"

// =============================================================================
//  CommandProcessor — CAMADA DE SESSAO (Fase 6)
//
//  Traduz UMA linha de texto da gramata do exercicio em chamadas ao nucleo, e
//  emite os eventos de PROTOCOLO ("Order created", "Order cancelled", erros) que
//  nao pertencem ao matching em si. E' o UNICO parser: o REPL (stdin) e o Gateway
//  (ZMQ) usam exatamente este mesmo caminho, garantindo que a rede se comporte
//  igual ao terminal.
//
//  Design: possui a engine e compartilha o MESMO sink com ela. Assim TODA a saida
//  (Trade vindo do nucleo + Order created/cancelled vindo daqui) flui por um so'
//  canal — o REPL manda p/ cout, o Gateway publica no socket PUB.
//
//  Gramatica aceita (case-insensitive no verbo/side):
//    limit  <buy|sell> <preco> <qty>
//    market <buy|sell> <qty>
//    cancel order <id>
//    modify order <id> price <preco>
//    modify order <id> qty <qty>
//    modify order <id> <preco> <qty>
//    peg bid   <buy|sell> <qty>      (segue o melhor bid)
//    peg offer <buy|sell> <qty>      (segue o melhor offer)
//    print book   |   print_book   |   book
// =============================================================================
class CommandProcessor {
public:
    using Sink = MatchingEngine::ReportSink;

    explicit CommandProcessor(Sink sink);

    // Parseia e executa UMA linha. Linhas vazias / comentarios ('#') sao ignorados.
    void process_line(const std::string& linha);

    // Acesso ao nucleo (usado por testes p/ inspecao direta).
    MatchingEngine& engine() { return engine_; }

private:
    Sink           sink_;     // mesmo canal da engine (saida unificada)
    MatchingEngine engine_;

    void emitir(const std::string& linha) const;  // manda uma linha ao sink

    // Converte "10" / "9.99" / "10.1" em CENTAVOS (uint32_t). Devolve false se
    // a string nao for um preco valido.
    static bool parse_side(const std::string& tok, Side& out);
    static bool parse_price(const std::string& tok, uint32_t& out);

    static std::string side_para_texto(Side side);
};
