// =============================================================================
//  test_journal.cpp — valida a FASE B (AsyncJournal: ping-pong + thread de drain).
//
//  Estrategia: usamos arenas MINUSCULAS (dezenas de bytes) de proposito, p/ FORCAR
//  milhares de rotacoes de arena e, com folga, disparar o caminho de BACKPRESSURE
//  (produtor esperando a arena drenar). Depois relemos o arquivo e conferimos as
//  duas propriedades que importam num log de auditoria:
//     (1) NENHUM evento se perde  -> a contagem de linhas bate;
//     (2) a ORDEM e' preservada   -> linha i == o i-esimo evento gravado.
//  Assim provamos que o desacoplamento hot-path/disco nao corrompe o historico.
// =============================================================================
#include "AsyncJournal.h"

#include <cstdio>    // std::remove
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static int g_total = 0;
static int g_pass  = 0;

static void checa(const std::string& nome, bool ok) {
    ++g_total;
    if (ok) ++g_pass;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << nome << "\n";
}

// Le todas as linhas de um arquivo de volta p/ um vetor.
static std::vector<std::string> ler_linhas(const std::string& path) {
    std::vector<std::string> linhas;
    std::ifstream in(path, std::ios::binary);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        linhas.push_back(l);
    }
    return linhas;
}

// =============================================================================
//  Teste 1 — estresse: N eventos, arenas minusculas -> muitas rotacoes.
// =============================================================================
static void teste_estresse_ordem_e_integridade() {
    std::cout << "\n== Teste 1: estresse (ping-pong sob rotacao intensa) ==\n";

    const std::string path = "test_journal_out.tsv";
    std::remove(path.c_str());  // comeca limpo (o AsyncJournal abre em modo append)

    const int N = 20000;
    std::uint64_t rot = 0, bp = 0;
    {
        // 48 bytes por arena: cabem poucas linhas -> rotaciona o tempo todo.
        AsyncJournal j(path, 48);
        checa("arquivo abriu p/ escrita", j.ok());
        for (int i = 0; i < N; ++i) {
            j.record("EV", "linha " + std::to_string(i));
        }
        rot = j.rotacoes();
        bp  = j.esperas_backpressure();
    }  // destrutor: entrega a arena parcial, faz join da thread, flush do arquivo

    const std::vector<std::string> linhas = ler_linhas(path);

    checa("contagem de linhas == N (nenhum evento perdido)",
          linhas.size() == static_cast<std::size_t>(N));

    bool ordem_ok = (linhas.size() == static_cast<std::size_t>(N));
    for (std::size_t i = 0; ordem_ok && i < linhas.size(); ++i) {
        ordem_ok = (linhas[i] == "EV\tlinha " + std::to_string(i));
    }
    checa("ordem preservada (linha i == i-esimo evento)", ordem_ok);

    // Estas duas nao sao "falhas" se nao dispararem, mas com arena de 48 bytes e
    // 20k linhas elas praticamente sempre disparam — sao a prova de que o caminho
    // de rotacao e o de backpressure foram REALMENTE exercitados.
    checa("houve rotacao de arena (o ping-pong rodou)", rot > 0);
    std::cout << "      [info] rotacoes=" << rot
              << "  esperas_backpressure=" << bp << "\n";
}

// =============================================================================
//  Teste 2 — fallback: linha MAIOR que a arena inteira nao pode se perder.
// =============================================================================
static void teste_linha_maior_que_arena() {
    std::cout << "\n== Teste 2: linha gigante (fallback de arena sob medida) ==\n";

    const std::string path = "test_journal_big.tsv";
    std::remove(path.c_str());

    const std::string pequena = "abc";
    const std::string gigante(500, 'X');  // >> 32 bytes de arena
    {
        AsyncJournal j(path, 32);
        j.record("EV", pequena);
        j.record("EV", gigante);   // nao cabe em 32 bytes -> caminho de fallback
        j.record("EV", pequena);
    }

    const std::vector<std::string> linhas = ler_linhas(path);
    const bool ok = linhas.size() == 3
                 && linhas[0] == "EV\t" + pequena
                 && linhas[1] == "EV\t" + gigante
                 && linhas[2] == "EV\t" + pequena;
    checa("linha gigante persistida na ordem certa, sem perder as vizinhas", ok);
}

int main() {
    std::cout << "===== TESTES DA FASE B (AsyncJournal) =====\n";
    teste_estresse_ordem_e_integridade();
    teste_linha_maior_que_arena();

    std::cout << "\n===== RESUMO: " << g_pass << "/" << g_total
              << " casos passaram =====\n";
    return (g_pass == g_total) ? 0 : 1;
}
