#include "ArenaAllocator.h"
#include <cstdlib>

// =============================================================================
//  ArenaAllocator — bloco contiguo pre-alocado; alocar() so' avanca um ponteiro.
//
//  FASE B: esta arena e' a unidade do ping-pong do AsyncJournal. Por isso o
//  resetar() aqui e' O(1) PURO (so' zera o offset) — NAO grava em disco. Quem
//  persiste e' a thread de drain, lendo getBuffer()/getUsedBytes() da arena cheia
//  ANTES de chamar resetar(). Assim o caminho quente nunca toca o I/O.
//
//  (A versao anterior fazia um dump sincrono dentro do resetar(); isso reintroduz
//  exatamente a latencia de disco que a Fase B existe pra eliminar — removido.)
// =============================================================================

ArenaAllocator::ArenaAllocator(size_t tamanho_em_bytes)
    : buffer_inicio(nullptr), tamanho_total(tamanho_em_bytes), offset_atual(0) {
    buffer_inicio = static_cast<char*>(std::malloc(tamanho_total));
}

ArenaAllocator::~ArenaAllocator() {
    std::free(buffer_inicio);
}

// O(1): avanca o ponteiro. Devolve nullptr se nao cabe (sinal p/ o AsyncJournal
// rotacionar de arena).
void* ArenaAllocator::alocar(size_t bytes_necessarios) {
    if (offset_atual + bytes_necessarios > tamanho_total) {
        return nullptr;
    }
    void* ptr = buffer_inicio + offset_atual;
    offset_atual += bytes_necessarios;
    return ptr;
}

// O(1): "esvazia" a arena reaproveitando o mesmo bloco (nenhum free/malloc).
void ArenaAllocator::resetar() {
    offset_atual = 0;
}
