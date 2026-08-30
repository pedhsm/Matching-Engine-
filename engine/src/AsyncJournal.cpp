#include "AsyncJournal.h"

#include <cstring>   // std::strlen, std::memcpy

// -----------------------------------------------------------------------------
//  Construcao: abre o arquivo, cria as DUAS arenas do ping-pong (1 ativa + 1
//  livre) e sobe a thread de drain.
// -----------------------------------------------------------------------------
AsyncJournal::AsyncJournal(const std::string& caminho, std::size_t arena_bytes)
    : out_(caminho, std::ios::app),
      arquivo_ok_(out_.good()),
      arena_bytes_(arena_bytes) {
    ativa_ = std::make_unique<ArenaAllocator>(arena_bytes_);
    livres_.push_back(std::make_unique<ArenaAllocator>(arena_bytes_)); // o "outro" lado do ping-pong
    drain_thread_ = std::thread([this] { drain_loop(); });
}

// -----------------------------------------------------------------------------
//  Destruicao: entrega a arena ativa (mesmo parcial) p/ nao perder os eventos
//  finais, sinaliza parada, e faz join. So' entao o ofstream fecha (flush).
// -----------------------------------------------------------------------------
AsyncJournal::~AsyncJournal() {
    {
        std::lock_guard<std::mutex> g(mutex_);
        if (ativa_ && ativa_->getUsedBytes() > 0) {
            prontas_.push_back(std::move(ativa_));
        }
        stop_ = true;
    }
    cv_drain_.notify_all();
    if (drain_thread_.joinable()) drain_thread_.join();
}

void AsyncJournal::escrever_linha(char* dst, const char* kind, std::size_t klen,
                                  const std::string& payload, std::size_t need) {
    std::memcpy(dst, kind, klen);
    dst[klen] = '\t';
    std::memcpy(dst + klen + 1, payload.data(), payload.size());
    dst[need - 1] = '\n';
}

// -----------------------------------------------------------------------------
//  HOT-PATH. Serializa a linha na arena ativa. Custo tipico: 1 memcpy. Nenhum
//  malloc, nenhuma syscall, nenhum I/O — essa e' a razao de ser da Fase B.
// -----------------------------------------------------------------------------
void AsyncJournal::record(const char* kind, const std::string& payload) {
    const std::size_t klen = std::strlen(kind);
    const std::size_t need = klen + 1 + payload.size() + 1;  // kind \t payload \n

    char* dst = static_cast<char*>(ativa_->alocar(need));
    if (!dst) {
        // Arena cheia: troca de lado (pode bloquear = backpressure) e tenta de novo.
        std::unique_lock<std::mutex> lk(mutex_);
        rotacionar(lk);
        lk.unlock();
        dst = static_cast<char*>(ativa_->alocar(need));
    }

    if (dst) {
        escrever_linha(dst, kind, klen, payload, need);
        return;
    }

    // Caso patologico: a linha e' MAIOR que uma arena inteira. Mantemos a
    // invariante "o produtor nunca escreve no arquivo": criamos uma arena sob
    // medida, preenchemos e entregamos direto p/ a fila de drain. Como isto so'
    // ocorre logo APOS um rotacionar() (a arena ativa acabou de ser trocada por
    // uma vazia), a ordem no disco continua correta. Na pratica nao acontece: as
    // linhas do journal tem dezenas de bytes; a arena tem dezenas de KiB.
    auto grande = std::make_unique<ArenaAllocator>(need);
    dst = static_cast<char*>(grande->alocar(need));
    escrever_linha(dst, kind, klen, payload, need);
    {
        std::lock_guard<std::mutex> g(mutex_);
        prontas_.push_back(std::move(grande));
    }
    cv_drain_.notify_one();
}

// -----------------------------------------------------------------------------
//  Troca de arena. Entrega a ativa (cheia) p/ drain e adota uma livre; se nao
//  houver livre, ESPERA (backpressure). Chamada com 'mutex_' ja' seguro por 'lk'.
// -----------------------------------------------------------------------------
void AsyncJournal::rotacionar(std::unique_lock<std::mutex>& lk) {
    prontas_.push_back(std::move(ativa_));
    rotacoes_.fetch_add(1, std::memory_order_relaxed);
    cv_drain_.notify_one();

    if (livres_.empty()) {
        esperas_backpressure_.fetch_add(1, std::memory_order_relaxed);
        cv_free_.wait(lk, [this] { return !livres_.empty(); });
    }
    ativa_ = std::move(livres_.back());
    livres_.pop_back();
}

// -----------------------------------------------------------------------------
//  Thread consumidora. Grava cada arena cheia FORA do lock (o I/O nao segura o
//  produtor), reseta O(1), e devolve a arena vazia ao pool. Termina somente com
//  stop_ pedido E a fila ja' vazia — ninguem perde eventos no shutdown.
// -----------------------------------------------------------------------------
void AsyncJournal::drain_loop() {
    std::unique_lock<std::mutex> lk(mutex_);
    while (true) {
        cv_drain_.wait(lk, [this] { return !prontas_.empty() || stop_; });

        while (!prontas_.empty()) {
            ArenaPtr arena = std::move(prontas_.front());
            prontas_.pop_front();

            lk.unlock();  // <<< I/O fora do lock: o produtor pode seguir preenchendo
            if (arquivo_ok_.load(std::memory_order_relaxed)) {
                out_.write(arena->getBuffer(),
                           static_cast<std::streamsize>(arena->getUsedBytes()));
                if (!out_.good()) arquivo_ok_.store(false, std::memory_order_relaxed);
            }
            arena->resetar();  // O(1)
            lk.lock();

            livres_.push_back(std::move(arena));
            cv_free_.notify_one();  // libera o produtor se ele estava em backpressure
        }

        if (stop_) break;  // so' sai com a fila vazia (garantido pelo while acima)
    }
    out_.flush();
}
