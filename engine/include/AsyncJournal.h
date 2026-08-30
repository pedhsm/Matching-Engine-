#pragma once

#include <string>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <memory>
#include <atomic>
#include <cstdint>

#include "ArenaAllocator.h"

// =============================================================================
//  AsyncJournal — FASE B do historico em disco (assincrono, ping-pong + drain).
//
//  MESMA interface do Journal (Fase A): record(kind, payload) + ok(). E' drop-in
//  no REPL e no Gateway. A diferenca esta' embaixo:
//
//    Fase A: record() chama ofstream na thread que casa ordens  -> I/O no HOT-PATH.
//    Fase B: record() so' faz um memcpy numa arena em memoria    -> ~O(1), zero I/O.
//            Uma THREAD DE DRAIN separada e' quem grava em disco.
//
//  PING-PONG (2 arenas):
//    - o produtor (hot-path) preenche a arena ATIVA;
//    - quando ela enche, entrega-a p/ a fila de drain e passa a preencher a OUTRA;
//    - a thread de drain grava a arena cheia em disco FORA de qualquer lock, faz
//      resetar() O(1), e devolve a arena vazia p/ o pool de livres.
//
//  BACKPRESSURE (a parte delicada): se o produtor enche a arena ativa e a outra
//  AINDA nao foi drenada (drain lento / disco lento), o produtor BLOQUEIA ate uma
//  arena ficar livre. E' a escolha correta p/ um log de AUDITORIA: preferimos uma
//  pausa rara a PERDER um evento. Dimensionando a arena p/ caber uma rajada tipica,
//  o bloqueio praticamente nunca acontece (ver contador de esperas). As outras
//  saidas — descartar (perde auditoria) ou alocar arenas sem limite (OOM) — sao
//  piores; por isso, backpressure.
//
//  INVARIANTE-CHAVE: o PRODUTOR NUNCA escreve no arquivo. Ele so' preenche arenas
//  e as entrega. Toda escrita passa pela thread de drain, na ordem FIFO da fila —
//  o que garante que a ordem dos eventos no disco == ordem em que chegaram.
//
//  Concorrencia: 1 produtor (single-thread, como o REPL e o loop do Gateway) e 1
//  consumidor. So' a fila de arenas e' compartilhada (protegida por mutex); as
//  arenas em si nunca sao tocadas por duas threads ao mesmo tempo.
// =============================================================================
class AsyncJournal {
public:
    // arena_bytes: tamanho de CADA arena do ping-pong. 64 KiB por padrao —
    // milhares de linhas curtas por rotacao, drain raro.
    explicit AsyncJournal(const std::string& caminho, std::size_t arena_bytes = (1u << 16));
    ~AsyncJournal();

    AsyncJournal(const AsyncJournal&)            = delete;
    AsyncJournal& operator=(const AsyncJournal&) = delete;

    // true se o arquivo abriu p/ escrita.
    bool ok() const { return arquivo_ok_.load(std::memory_order_relaxed); }

    // HOT-PATH (chamado pela thread do produtor). Serializa "<kind>\t<payload>\n"
    // direto na arena ativa. kind ∈ {"IN","EV"} (feed / evento).
    void record(const char* kind, const std::string& payload);

    // --- Telemetria (util p/ demonstrar o comportamento na defesa/testes) -----
    std::uint64_t rotacoes()            const { return rotacoes_.load();            }
    std::uint64_t esperas_backpressure() const { return esperas_backpressure_.load(); }

private:
    using ArenaPtr = std::unique_ptr<ArenaAllocator>;

    // Entrega a arena ativa (mesmo parcial) p/ a fila de drain e pega uma vazia
    // do pool de livres — bloqueando (backpressure) se nao houver nenhuma. Assume
    // que 'lk' ja' segura o mutex_.
    void rotacionar(std::unique_lock<std::mutex>& lk);

    // Corpo da thread de drain: espera arena cheia, grava fora do lock, reseta e
    // devolve ao pool. So' termina quando stop_ && fila vazia.
    void drain_loop();

    static void escrever_linha(char* dst, const char* kind, std::size_t klen,
                               const std::string& payload, std::size_t need);

    std::ofstream       out_;
    std::atomic<bool>   arquivo_ok_;
    std::size_t         arena_bytes_;

    ArenaPtr                ativa_;    // arena que o produtor preenche AGORA
    std::deque<ArenaPtr>    prontas_;  // cheias, esperando drain (FIFO = ordem)
    std::vector<ArenaPtr>   livres_;   // vazias, prontas p/ reuso

    std::mutex              mutex_;     // protege prontas_/livres_/stop_
    std::condition_variable cv_drain_;  // acorda a thread de drain
    std::condition_variable cv_free_;   // acorda o produtor preso em backpressure
    bool                    stop_ = false;
    std::thread             drain_thread_;

    std::atomic<std::uint64_t> rotacoes_{0};
    std::atomic<std::uint64_t> esperas_backpressure_{0};
};
