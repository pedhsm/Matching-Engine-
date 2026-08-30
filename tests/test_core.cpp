// =============================================================================
//  test_core.cpp — valida o NUCLEO da matching engine (Fases 1-3).
//
//  Estrategia: injetamos um SINK coletor (guarda cada linha reportada num vetor).
//  Depois de cada comando, "drenamos" o coletor e comparamos com o esperado.
//  Assim testamos a LOGICA sem depender de cout/rede — exatamente o ganho do
//  desacoplamento de I/O.
// =============================================================================
#include "MatchingEngine.h"

#include <iostream>
#include <string>
#include <vector>

// --- Micro-framework de teste (sem dependencias externas) --------------------
static int g_total = 0;
static int g_pass  = 0;

// Coletor: um sink que empilha as linhas reportadas; drain() devolve e limpa.
struct Coletor {
    std::vector<std::string> linhas;
    // Retorna uma std::function que captura ESTE coletor por ponteiro.
    MatchingEngine::ReportSink sink() {
        return [this](const std::string& l) { linhas.push_back(l); };
    }
    std::vector<std::string> drain() {
        std::vector<std::string> copia = linhas;
        linhas.clear();
        return copia;
    }
};

static void imprime_vetor(const char* rotulo, const std::vector<std::string>& v) {
    std::cout << "      " << rotulo << ": [";
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::cout << '"' << v[i] << '"';
        if (i + 1 < v.size()) std::cout << ", ";
    }
    std::cout << "]\n";
}

// Compara o que saiu com o esperado e imprime PASS/FAIL.
static void checa(const std::string& nome,
                  const std::vector<std::string>& obtido,
                  const std::vector<std::string>& esperado) {
    ++g_total;
    const bool ok = (obtido == esperado);
    if (ok) ++g_pass;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << nome << "\n";
    if (!ok) {
        imprime_vetor("esperado", esperado);
        imprime_vetor("obtido  ", obtido);
    }
}

// Versao "contem": passa se ALGUMA linha obtida for igual ao alvo.
static void checa_contem(const std::string& nome,
                         const std::vector<std::string>& obtido,
                         const std::string& alvo) {
    ++g_total;
    bool achou = false;
    for (const auto& l : obtido) if (l == alvo) { achou = true; break; }
    if (achou) ++g_pass;
    std::cout << (achou ? "[PASS] " : "[FAIL] ") << nome << "\n";
    if (!achou) {
        std::cout << "      esperava conter: \"" << alvo << "\"\n";
        imprime_vetor("obtido  ", obtido);
    }
}

// =============================================================================
//  Teste 1 — cenario EXATO do e-mail
// =============================================================================
static void teste_cenario_do_email() {
    std::cout << "\n== Teste 1: cenario exato do e-mail ==\n";
    Coletor c;
    MatchingEngine eng(c.sink());

    // Precos em CENTAVOS: 10 -> 1000, 20 -> 2000.
    eng.on_limit_order(Side::BUY,  1000, 100);  // limit buy 10 100  (descansa)
    eng.on_limit_order(Side::SELL, 2000, 100);  // limit sell 20 100 (descansa)
    eng.on_limit_order(Side::SELL, 2000, 200);  // limit sell 20 200 (descansa)
    checa("3 limits iniciais nao geram trade", c.drain(), {});

    eng.on_market_order(Side::BUY, 150);  // consome 100+50 @ 20 -> 1 linha agregada
    checa("market buy 150", c.drain(), {"Trade, price: 20, qty: 150"});

    eng.on_market_order(Side::BUY, 200);  // so' restam 150 @ 20; resto (50) descartado
    checa("market buy 200 (resto descartado)", c.drain(), {"Trade, price: 20, qty: 150"});

    eng.on_market_order(Side::SELL, 200); // so' ha 100 no bid @ 10; resto descartado
    checa("market sell 200 (resto descartado)", c.drain(), {"Trade, price: 10, qty: 100"});
}

// =============================================================================
//  Teste 2 — print_book (formato do e-mail, uma linha por ordem)
// =============================================================================
static void teste_print_book() {
    std::cout << "\n== Teste 2: print_book ==\n";
    Coletor c;
    MatchingEngine eng(c.sink());

    // Monta o livro do exemplo do e-mail:
    //   bids: 200 @ 10, 100 @ 9.99   |   asks: 100 @ 10.5
    eng.on_limit_order(Side::BUY,  1000, 200);  // 10.00
    eng.on_limit_order(Side::BUY,   999, 100);  // 9.99
    eng.on_limit_order(Side::SELL, 1050, 100);  // 10.50  (nao cruza: melhor bid 10 < 10.5)
    c.drain();  // descarta qualquer coisa; nao deve ter havido trade

    eng.print_book();
    checa("print_book layout", c.drain(), {
        "Ordens de Compra | Ordens de Venda",
        "-----------------|-----------------",
        "200 @ 10         | 100 @ 10.5",
        "100 @ 9.99       |",
    });
}

// =============================================================================
//  Teste 3 — marketable-limit (a limit agressiva PREENCHE, resto descansa)
// =============================================================================
static void teste_marketable_limit() {
    std::cout << "\n== Teste 3: marketable-limit ==\n";

    // (a) ask 100 @ 20 no livro; limit buy 20 60 casa 60 @ 20; nada descansa.
    {
        Coletor c;
        MatchingEngine eng(c.sink());
        eng.on_limit_order(Side::SELL, 2000, 100);  // ask passiva 100 @ 20
        c.drain();
        eng.on_limit_order(Side::BUY, 2000, 60);     // agressiva: casa 60 @ 20
        checa("limit buy 20 60 casa 60@20", c.drain(), {"Trade, price: 20, qty: 60"});
    }

    // (b) limit buy 25 300 cruza VARIOS niveis (<=25), casa o que da e o resto
    //     (100) descansa a 25 como novo melhor bid.
    {
        Coletor c;
        MatchingEngine eng(c.sink());
        eng.on_limit_order(Side::SELL, 2000, 100);  // ask 100 @ 20
        eng.on_limit_order(Side::SELL, 2200, 100);  // ask 100 @ 22
        c.drain();

        eng.on_limit_order(Side::BUY, 2500, 300);   // limit buy 25 300
        checa("limit buy 25 300 atravessa 2 niveis", c.drain(), {
            "Trade, price: 20, qty: 100",
            "Trade, price: 22, qty: 100",
        });

        // O resto (100) deve ter descansado como bid a 25.
        eng.print_book();
        checa_contem("resto de 100 descansa como bid @ 25", c.drain(), "100 @ 25         |");
    }
}

// =============================================================================
//  Teste 4 — format_price (funcao pura, casos de borda)
// =============================================================================
static void teste_format_price() {
    std::cout << "\n== Teste 4: format_price ==\n";
    struct Caso { uint32_t centavos; const char* esperado; };
    const Caso casos[] = {
        {1000, "10"}, {1050, "10.5"}, {999, "9.99"},
        {1010, "10.1"}, {1001, "10.01"}, {1005, "10.05"}, {0, "0"},
    };
    for (const auto& caso : casos) {
        ++g_total;
        const std::string got = MatchingEngine::format_price(caso.centavos);
        const bool ok = (got == caso.esperado);
        if (ok) ++g_pass;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << "format_price("
                  << caso.centavos << ") = \"" << got << "\"";
        if (!ok) std::cout << "  (esperado \"" << caso.esperado << "\")";
        std::cout << "\n";
    }
}

int main() {
    std::cout << "===== TESTES DO NUCLEO DA MATCHING ENGINE =====\n";
    teste_cenario_do_email();
    teste_print_book();
    teste_marketable_limit();
    teste_format_price();

    std::cout << "\n===== RESUMO: " << g_pass << "/" << g_total
              << " casos passaram =====\n";
    return (g_pass == g_total) ? 0 : 1;
}
