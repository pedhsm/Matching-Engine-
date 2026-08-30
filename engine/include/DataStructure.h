#pragma once
#include <cstdint>

// sell = 0 and buy =1 
enum class Side: uint8_t {
    SELL = 0,
    BUY = 1
};

// easier to operate than using strings 
enum class Type:uint8_t {
    LIMIT = 0,
    MARKET = 1
};


// Registro de UMA ordem viva no livro. Mantido em POD (sem métodos/construtores)
// de propósito: fica trivial de copiar/serializar e barato de guardar em um
// std::vector contíguo (bom p/ cache e p/ o journaling/arena das fases futuras).
struct EngineOrder{
    uint64_t order_id;   // id incremental, tambem usado como indice no order_pool
    uint64_t timestamp;  // ordem de chegada; a prioridade-tempo real e' garantida
                         // estruturalmente pela fila (std::list) — o campo fica p/ auditoria
    uint64_t qty;        // quantidade AINDA nao preenchida (cai a cada fill parcial)
    uint32_t price;      // preco em CENTAVOS (inteiro) p/ evitar erro de ponto flutuante
    Type type;
    Side side;
}; // <- o ';' faltava no esqueleto original (erro de compilacao)

