#pragma once

#include <fstream>
#include <string>

// =============================================================================
//  Journal — FASE A do histórico em disco (síncrono, bufferizado)
//
//  Grava um log append-only de tudo que passa pela sessão: o FEED (comandos que
//  chegam) e os EVENTOS que a engine emite (Order created/cancelled, Trade...).
//  Serve pra consultar o histórico depois — o formato é TSV, então cai direto no
//  pandas: pd.read_csv("journal.tsv", sep="\t", names=["kind","payload"]).
//
//  Por que ofstream e NÃO um arena allocator aqui: no caminho SÍNCRONO o próprio
//  std::ofstream já tem buffer interno e escreve em disco em blocos — batelamento
//  de graça. Uma arena só se pagaria no caminho ASSÍNCRONO (ping-pong + thread de
//  drain), que é a Fase B, documentada no README como o upgrade de latência.
//
//  Usa TSV (e não CSV) de propósito: os payloads têm vírgulas ("Trade, price: 20,
//  qty: 150"), mas nunca têm TAB — então não precisa de escaping.
// =============================================================================
class Journal {
public:
    explicit Journal(const std::string& caminho)
        : out_(caminho, std::ios::app) {}

    // true se o arquivo abriu pra escrita.
    bool ok() const { return out_.good(); }

    // Registra uma linha: <kind>\t<payload>. kind ∈ {"IN","EV"} (feed / evento).
    void record(const char* kind, const std::string& payload) {
        if (out_) out_ << kind << '\t' << payload << '\n';
    }

private:
    std::ofstream out_;
};
