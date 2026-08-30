#include "CommandProcessor.h"

#include <sstream>
#include <vector>
#include <algorithm>  // std::transform
#include <cctype>     // std::tolower

// =============================================================================
//  Construtor: a engine recebe o MESMO sink -> saida unificada (Trade + eventos
//  de protocolo saem pelo mesmo canal).
// =============================================================================
CommandProcessor::CommandProcessor(Sink sink)
    : sink_(sink), engine_(sink) {}

void CommandProcessor::emitir(const std::string& linha) const {
    if (sink_) sink_(linha);
}

std::string CommandProcessor::side_para_texto(Side side) {
    return (side == Side::BUY) ? "buy" : "sell";
}

// -----------------------------------------------------------------------------
//  Parsing de side e preco
// -----------------------------------------------------------------------------
bool CommandProcessor::parse_side(const std::string& tok, Side& out) {
    if (tok == "buy")  { out = Side::BUY;  return true; }
    if (tok == "sell") { out = Side::SELL; return true; }
    return false;
}

// Converte preco decimal em CENTAVOS. Regras de borda documentadas no header.
bool CommandProcessor::parse_price(const std::string& tok, uint32_t& out) {
    if (tok.empty()) return false;
    const std::size_t ponto = tok.find('.');
    try {
        if (ponto == std::string::npos) {
            // Inteiro puro: "10" -> 1000 centavos.
            const unsigned long inteiro = std::stoul(tok);
            out = static_cast<uint32_t>(inteiro * 100);
            return true;
        }
        std::string parte_int = tok.substr(0, ponto);
        std::string parte_dec = tok.substr(ponto + 1);

        // Normaliza a parte decimal p/ exatamente 2 casas (centavos):
        //   "1"  -> "10" (pad a' DIREITA: 10.1 = 10 e 10 centavos)
        //   "99" -> "99"
        //   "059"-> "05" (trunca alem de centavos)
        if (parte_dec.size() > 2) parte_dec = parte_dec.substr(0, 2);
        while (parte_dec.size() < 2) parte_dec += '0';

        const unsigned long inteiro  = parte_int.empty() ? 0 : std::stoul(parte_int);
        const unsigned long centavos = std::stoul(parte_dec);
        out = static_cast<uint32_t>(inteiro * 100 + centavos);
        return true;
    } catch (...) {
        return false;  // stoul lancou (nao-numerico / overflow)
    }
}

// =============================================================================
//  process_line — tokeniza e despacha
// =============================================================================
void CommandProcessor::process_line(const std::string& linha) {
    // Tokeniza por espacos em branco.
    std::istringstream iss(linha);
    std::vector<std::string> tok;
    for (std::string t; iss >> t;) tok.push_back(t);

    if (tok.empty()) return;                 // linha vazia
    if (tok[0].size() && tok[0][0] == '#') return;  // comentario

    // Verbo em minusculas (aceita LIMIT/Limit/limit etc.).
    std::string verbo = tok[0];
    std::transform(verbo.begin(), verbo.end(), verbo.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto erro = [&](const std::string& msg) { emitir("Error: " + msg); };

    // --- limit <buy|sell> <preco> <qty> ------------------------------------
    if (verbo == "limit") {
        if (tok.size() != 4) return erro("uso: limit <buy|sell> <preco> <qty>");
        Side side; uint32_t preco;
        if (!parse_side(tok[1], side))  return erro("side invalido: " + tok[1]);
        if (!parse_price(tok[2], preco)) return erro("preco invalido: " + tok[2]);
        uint64_t qty;
        try { qty = std::stoull(tok[3]); } catch (...) { return erro("qty invalida"); }

        const int64_t id = engine_.on_limit_order(side, preco, qty);
        // "Order created" so' faz sentido se ALGO descansou no livro. Mostramos a
        // qty que DE FATO restou (apos eventuais fills parciais) e o preco-limite.
        if (id >= 0) {
            emitir("Order created: " + side_para_texto(side) + " " +
                   std::to_string(engine_.qty_of(static_cast<uint64_t>(id))) + " @ " +
                   MatchingEngine::format_price(preco) + " " + std::to_string(id));
        }
        return;
    }

    // --- market <buy|sell> <qty> -------------------------------------------
    if (verbo == "market") {
        if (tok.size() != 3) return erro("uso: market <buy|sell> <qty>");
        Side side;
        if (!parse_side(tok[1], side)) return erro("side invalido: " + tok[1]);
        uint64_t qty;
        try { qty = std::stoull(tok[2]); } catch (...) { return erro("qty invalida"); }
        engine_.on_market_order(side, qty);  // so' emite Trade (via sink da engine)
        return;
    }

    // --- cancel order <id> -------------------------------------------------
    if (verbo == "cancel") {
        // Aceita "cancel order <id>" e tambem "cancel <id>".
        std::string id_txt;
        if (tok.size() == 3 && tok[1] == "order") id_txt = tok[2];
        else if (tok.size() == 2)                 id_txt = tok[1];
        else return erro("uso: cancel order <id>");

        uint64_t id;
        try { id = std::stoull(id_txt); } catch (...) { return erro("id invalido"); }

        if (engine_.cancel(id)) emitir("Order cancelled");
        else                    emitir("Order not found: " + id_txt);
        return;
    }

    // --- modify order <id> ... ---------------------------------------------
    if (verbo == "modify") {
        // Formas: modify order <id> price <p>
        //         modify order <id> qty <q>
        //         modify order <id> <p> <q>
        if (tok.size() < 4 || tok[1] != "order")
            return erro("uso: modify order <id> [price <p>] [qty <q>] | <p> <q>");

        uint64_t id;
        try { id = std::stoull(tok[2]); } catch (...) { return erro("id invalido"); }

        std::optional<uint32_t> novo_preco;
        std::optional<uint64_t> nova_qty;

        // Varre os tokens a partir de tok[3] procurando "price <p>", "qty <q>" ou
        // dois numeros posicionais (<p> <q>).
        std::vector<std::string> pos_numericos;
        for (std::size_t i = 3; i < tok.size(); ++i) {
            if (tok[i] == "price" && i + 1 < tok.size()) {
                uint32_t p; if (!parse_price(tok[++i], p)) return erro("preco invalido");
                novo_preco = p;
            } else if (tok[i] == "qty" && i + 1 < tok.size()) {
                try { nova_qty = std::stoull(tok[++i]); } catch (...) { return erro("qty invalida"); }
            } else {
                pos_numericos.push_back(tok[i]);
            }
        }
        // Forma posicional: <p> <q> (so' se nenhum keyword foi usado p/ o mesmo campo).
        if (!pos_numericos.empty()) {
            if (pos_numericos.size() >= 1 && !novo_preco) {
                uint32_t p; if (!parse_price(pos_numericos[0], p)) return erro("preco invalido");
                novo_preco = p;
            }
            if (pos_numericos.size() >= 2 && !nova_qty) {
                try { nova_qty = std::stoull(pos_numericos[1]); } catch (...) { return erro("qty invalida"); }
            }
        }

        if (!novo_preco && !nova_qty) return erro("modify sem campos");
        if (engine_.modify(id, novo_preco, nova_qty)) emitir("Order modified");
        else                                          emitir("Order not found: " + tok[2]);
        return;
    }

    // --- peg bid|offer <buy|sell> <qty> ------------------------------------
    if (verbo == "peg") {
        if (tok.size() != 4) return erro("uso: peg <bid|offer> <buy|sell> <qty>");
        bool peg_to_bid;
        if      (tok[1] == "bid")   peg_to_bid = true;
        else if (tok[1] == "offer") peg_to_bid = false;
        else return erro("referencia invalida (use bid|offer): " + tok[1]);

        Side side;
        if (!parse_side(tok[2], side)) return erro("side invalido: " + tok[2]);
        uint64_t qty;
        try { qty = std::stoull(tok[3]); } catch (...) { return erro("qty invalida"); }

        const int64_t id = engine_.on_pegged_order(side, qty, peg_to_bid);
        // Anuncia a pegged (dá um id cancelavel ao cliente). O preco mostrado e' o
        // preco de referencia atual (0 se ainda nao ha referencia p/ acompanhar).
        emitir("Order created: peg " + std::string(peg_to_bid ? "bid " : "offer ") +
               side_para_texto(side) + " " + std::to_string(qty) + " " +
               std::to_string(id));
        return;
    }

    // --- print book / book -------------------------------------------------
    if ((verbo == "print" && tok.size() >= 2 && tok[1] == "book") ||
        verbo == "print_book" || verbo == "book") {
        engine_.print_book();
        return;
    }

    erro("comando desconhecido: " + tok[0]);
}
