#include "ArenaAllocator.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <chrono>

ArenaAllocator::ArenaAllocator(size_t tamanho_em_bytes)
    : buffer_inicio(nullptr), tamanho_total(tamanho_em_bytes), offset_atual(0) {
    buffer_inicio = static_cast<char*>(std::malloc(tamanho_total));
}

ArenaAllocator::~ArenaAllocator() {
    std::free(buffer_inicio);
}

void* ArenaAllocator::alocar(size_t bytes_necessarios) {
    if (offset_atual + bytes_necessarios > tamanho_total) {
        return nullptr; // Sem espaço
    }
    void* ptr = buffer_inicio + offset_atual;
    offset_atual += bytes_necessarios;
    return ptr;
}

void ArenaAllocator::resetar() {
    // Salvar o buffer atual em um arquivo antes de resetar (para consulta posterior)
    if (offset_atual > 0) {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        std::string filename = "arena_dump_" + std::to_string(ms) + ".dat";
        std::ofstream outfile(filename, std::ios::binary);
        if (outfile) {
            outfile.write(buffer_inicio, offset_atual);
            outfile.close();
        } else {
            std::cerr << "Erro ao salvar dump da arena: " << filename << std::endl;
        }
    }
    offset_atual = 0; // O(1) reset
}
