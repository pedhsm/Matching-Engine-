#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <list>
#include <functional>
#include <optional>

#include "DataStructure.h"

// =============================================================================
//  MatchingEngine — NUCLEO PURO (Fases 1 a 5)
//
//  Responsabilidade unica: manter o livro de ofertas de UM ativo e casar ordens
//  por prioridade PRECO-TEMPO. NAO conhece rede, ZMQ, threads nem parser — isso
//  e' de proposito (Single Responsibility): a engine so' decide "quem casa com
//  quem" e REPORTA os eventos por um sink injetado. Quem fala com o mundo (cout,
//  socket, journal) e' problema de outra camada (CommandProcessor / Gateway).
//
//  O que a engine REPORTA pelo sink: apenas eventos de EXECUCAO ("Trade, ...").
//  Eventos de ciclo-de-vida com identificador voltado ao cliente ("Order
//  created", "Order cancelled") sao emitidos pela camada de sessao
//  (CommandProcessor), que conhece o protocolo texto. Assim os testes do nucleo
//  (test_core.cpp) continuam validando so' o matching, sem ruido de protocolo.
// =============================================================================
class MatchingEngine {
public:
    // Canal de saida injetado. Todo evento textual de EXECUCAO (linhas de Trade,
    // o livro) sai por aqui. O teste injeta um coletor em memoria; o REPL injeta
    // std::cout; o Gateway ZMQ injeta um sink que publica no socket PUB. Isso
    // desacopla a LOGICA do I/O (testavel, reaproveitavel).
    using ReportSink = std::function<void(const std::string&)>;

    // pool_capacity so' pre-reserva memoria (evita realocacoes); nao e' um limite
    // rigido.
    explicit MatchingEngine(ReportSink sink, std::size_t pool_capacity = 1024);

    // --- API do nucleo -----------------------------------------------------

    // Ordem LIMITE: primeiro tenta casar (parte agressiva/marketable) contra o
    // lado oposto nos precos que satisfazem o limite; QUALQUER resto DESCANSA
    // como liquidez passiva no proprio preco-limite.
    // RETORNA o id da ordem que descansou (p/ a camada de sessao emitir "Order
    // created"), ou -1 se a ordem casou por inteiro e nada descansou.
    int64_t on_limit_order(Side side, uint32_t price, uint64_t qty);

    // Ordem A MERCADO: casa gulosamente contra o melhor preco do lado oposto ate
    // preencher a qty OU o lado esvaziar. O resto NAO preenchido e' DESCARTADO —
    // ordem market nunca descansa no livro.
    void on_market_order(Side side, uint64_t qty);

    // FASE 4 — cancelamento. Usa o locator p/ achar e remover a ordem SEM varrer
    // o livro (O(1) na fila; O(1) amortizado p/ apagar o nivel vazio). Devolve
    // true se cancelou, false se o id nao existe / ja saiu do livro.
    bool cancel(uint64_t id);

    // FASE 4 — alteracao de ordem (preco, qty, ou ambos). REGRAS:
    //   (a) SO' reducao de qty, mesmo preco  -> in-place, MANTEM prioridade-tempo;
    //   (b) aumento de qty OU mudanca de preco -> PERDE prioridade: a ordem e'
    //       removida e RE-SUBMETIDA como limit com o MESMO id (podendo casar se
    //       ficar marketable; senao descansa no fim da nova fila).
    // Passe std::nullopt no campo que nao muda. Devolve true se alterou.
    bool modify(uint64_t id,
                std::optional<uint32_t> novo_preco,
                std::optional<uint64_t> nova_qty);

    // FASE 5 — ordem PEGGED: acompanha dinamicamente um preco de referencia.
    //   peg_to_bid == true  -> segue o MELHOR BID  (peg to the bid)
    //   peg_to_bid == false -> segue o MELHOR OFFER (peg to the offer)
    // A ordem descansa como liquidez PASSIVA (nao agride) no preco de referencia
    // e e' RECOLOCADA sempre que a referencia muda (ver reavaliar_pegged()).
    // Devolve o id da pegged.
    int64_t on_pegged_order(Side side, uint64_t qty, bool peg_to_bid);

    // Requisito adicional 1: visualizacao do livro (uma linha por ORDEM).
    void print_book() const;

    // --- Consultas (usadas pela camada de sessao / testes) -----------------

    // Qty AINDA viva de uma ordem (p/ "Order created" mostrar o que de fato
    // descansou). 0 se o id nao existe.
    uint64_t qty_of(uint64_t id) const;

    // A ordem ainda esta' viva no livro? (nao foi preenchida nem cancelada)
    bool is_live(uint64_t id) const;

    // Converte preco em CENTAVOS p/ string "amigavel", sem zeros inuteis:
    //   1000 -> "10"   1050 -> "10.5"   999 -> "9.99"   1001 -> "10.01"
    // static porque so' depende do argumento (funcao pura), util em testes.
    static std::string format_price(uint32_t centavos_de_preco);

private:
    // Aliases dos tipos do livro. asks e bids sao mapas de TIPOS DIFERENTES (o
    // comparador muda o sentido da ordenacao), por isso precisam de aliases
    // distintos — e por isso o Locator guarda um iterador de nivel p/ cada lado.
    using NivelFila = std::list<uint64_t>;                       // FIFO de ids num nivel
    using AsksMap   = std::map<uint32_t, NivelFila>;            // asc: melhor ask = begin()
    using BidsMap   = std::map<uint32_t, NivelFila, std::greater<uint32_t>>; // desc: melhor bid = begin()

    // Locator: tudo que cancel/modify/pegged precisam p/ achar e remover uma
    // ordem SEM varrer o livro. Guardamos:
    //   - na_fila:   iterador da posicao exata da ordem dentro da FIFO do nivel
    //                -> remocao O(1) do elemento;
    //   - nivel_ask / nivel_bid: iterador do NIVEL no mapa correspondente ao side
    //                -> acesso a fila via ->second E remocao O(1) do nivel vazio.
    // Iteradores de std::list e de std::map sao ESTAVEIS: inserir/remover OUTROS
    // elementos nao os invalida (ambos sao baseados em nos). So' o iterador do
    // proprio elemento removido e' invalidado — por isso, ao preencher/cancelar,
    // marcamos 'ativo=false' e nunca mais tocamos nos iteradores desse locator.
    // Como asks/bids tem tipos diferentes, mantemos os dois iteradores de nivel,
    // mas so' o do 'side' e' valido.
    struct Locator {
        bool ativo = false;               // a ordem esta' viva no livro?
        Side side  = Side::BUY;           // qual lado (define qual nivel_* usar)
        NivelFila::iterator na_fila;      // posicao na FIFO do nivel
        AsksMap::iterator   nivel_ask;    // valido se side == SELL
        BidsMap::iterator   nivel_bid;    // valido se side == BUY
    };

    // Registro de UMA ordem pegged viva. A qty/preco/timestamp reais vivem em
    // order_pool[id]; aqui guardamos so' o que a engine precisa p/ re-pegar.
    struct Pegged {
        uint64_t id;
        bool     peg_to_bid;  // true = segue melhor bid; false = segue melhor offer
        bool     colocada;    // ja' foi inserida num nivel? (false enquanto nao ha referencia)
    };

    // --- Helpers internos ---------------------------------------------------

    // Consome o TOPO da fila (FIFO) de UM nivel, ate a qty agressora zerar ou a
    // fila esvaziar. Devolve quanto casou NESTE nivel. Ao preencher uma passiva
    // por inteiro, marca o locator dela como inativo (a ordem morreu no livro).
    uint64_t consumir_nivel(NivelFila& fila, uint64_t& qty_agressora);

    // Miolo compartilhado por on_limit_order e pela re-submissao do modify: casa
    // 'qty' contra o lado oposto e o RESTO descansa. Se id_forcado >= 0, reutiliza
    // esse id ao descansar (modify mantem a IDENTIDADE, so' perde a prioridade);
    // se id_forcado < 0, aloca um id novo. Devolve o id que descansou, ou -1.
    int64_t processar_limit(int64_t id_forcado, Side side, uint32_t price, uint64_t qty);

    // Coloca 'qty' como ordem PASSIVA no FIM da fila do nivel (push_back = menor
    // prioridade-tempo = timestamp fresco). id_forcado>=0 reutiliza o id (modify);
    // caso contrario aloca um id novo. Devolve o id.
    uint64_t descansar(Side side, uint32_t price, uint64_t qty, int64_t id_forcado);

    // Insere 'id' na fila do nivel 'price' do 'side' NA POSICAO POR TIMESTAMP
    // (menor timestamp = mais a' frente). Usado pela pegged, que PRESERVA seu
    // timestamp original ao re-pegar e por isso entra no MEIO da fila, a' frente
    // de ordens mais novas. Atualiza o locator.
    void inserir_por_timestamp(uint64_t id, Side side, uint32_t price);

    // Remove a ordem do seu nivel (via locator), apagando o nivel se ficar vazio.
    // NAO marca o locator como inativo — usado tanto pela remocao definitiva
    // (remover_do_livro) quanto pelo "mover" da pegged (que continua viva).
    void desanexar_do_nivel(uint64_t id);

    // Remocao DEFINITIVA do livro (cancel / modify-perde-prioridade): desanexa e
    // marca o locator inativo + a ordem morta.
    void remover_do_livro(uint64_t id);

    // Move uma pegged VIVA para 'novo_preco': desanexa do nivel atual, atualiza o
    // preco em order_pool e re-insere por timestamp no novo nivel.
    void mover_pegged(uint64_t id, uint32_t novo_preco);

    // Reavalia TODAS as pegged: apos qualquer operacao que possa mexer no topo,
    // se a referencia (melhor bid/offer) mudou, move a pegged. Idempotente (so'
    // move se o preco diferir) e em ponto-fixo (mover uma pode afetar outra).
    void reavaliar_pegged();

    // Envia uma linha ao sink (se houver). Centraliza o "so' a engine reporta".
    void reportar(const std::string& linha) const;

    // Coracao do matching. Percorre o livro OPOSTO do melhor p/ o pior preco,
    // casando FIFO e emitindo UMA linha de Trade por nivel. Template porque 'asks'
    // e 'bids' sao mapas de tipos diferentes; o template gera as duas versoes sem
    // duplicar o algoritmo. 'cruza' e' o predicado da regra de preco.
    template <typename LivroOposto, typename Cruza>
    void casar(LivroOposto& oposto, uint64_t& qty_agressora, Cruza cruza);

    // --- Estado ------------------------------------------------------------

    ReportSink report_sink;         // canal de saida (pode ser nulo)

    // Dados de cada ordem viva, indexado por order_id. std::vector contiguo:
    // O(1) por indice e amigavel a cache. IDs incrementais + nunca removemos do
    // meio => order_pool[id] fica valido p/ sempre (indice estavel).
    std::vector<EngineOrder> order_pool;

    // Duas SEQUENCIAS monotonicas SEPARADAS de proposito:
    //   next_order_id -> IDENTIDADE da ordem. So' avanca em ordem NOVA. Nunca muda
    //                    p/ uma ordem existente (o cancel usa o id, o cliente idem).
    //   next_seq      -> PRIORIDADE-TEMPO (timestamp). Avanca a cada colocacao que
    //                    estabelece prioridade fresca (ordem nova; re-submissao do
    //                    modify que "perde prioridade"). A pegged PRESERVA o seu.
    // Separar as duas e' o que permite ao modify manter a identidade e, ao mesmo
    // tempo, receber uma prioridade-tempo nova.
    uint64_t next_order_id = 0;
    uint64_t next_seq      = 0;

    // Asks (VENDAS): preco ASCENDENTE. Melhor ask = menor preco = begin().
    AsksMap asks;
    // Bids (COMPRAS): preco DESCENDENTE. Melhor bid = maior preco = begin().
    BidsMap bids;

    // id -> Locator. Preenchido ao descansar/inserir; lido pelo cancel/modify e
    // invalidado (ativo=false) ao preencher/cancelar.
    std::vector<Locator> order_locators;

    // Registro das pegged vivas (varrido por reavaliar_pegged).
    std::vector<Pegged> pegged_ativas;
};

// -----------------------------------------------------------------------------
//  Definicao do metodo-template 'casar'. Fica no header (fora do .cpp) porque
//  templates precisam da definicao visivel em cada ponto de instanciacao.
// -----------------------------------------------------------------------------
template <typename LivroOposto, typename Cruza>
void MatchingEngine::casar(LivroOposto& oposto, uint64_t& qty_agressora, Cruza cruza) {
    // Percorre os niveis do MELHOR p/ o PIOR preco. begin() e' o topo nos dois
    // mapas (o comparador de cada um ja ordena no sentido certo).
    while (qty_agressora > 0 && !oposto.empty()) {
        auto nivel = oposto.begin();
        const uint32_t preco = nivel->first;

        // 'cruza' decide se o preco do topo ainda satisfaz a ordem que chega:
        //   market      -> sempre true (casa contra qualquer preco)
        //   limit buy   -> preco_do_ask  <= limite
        //   limit sell  -> preco_do_bid  >= limite
        // Se o MELHOR preco ja nao cruza, nenhum pior vai cruzar: paramos.
        if (!cruza(preco)) break;

        // Casa quantas ordens couberem NESTE nivel (mesma prioridade de preco),
        // respeitando FIFO. Retorna a qty total casada aqui.
        const uint64_t casada_no_nivel = consumir_nivel(nivel->second, qty_agressora);

        // AGREGACAO POR PRECO (exigido pelo e-mail): UMA linha de Trade por nivel,
        // somando a qty. Ex.: casar 100+50 a 20 imprime "Trade, price: 20, qty: 150".
        if (casada_no_nivel > 0) {
            reportar("Trade, price: " + format_price(preco) +
                     ", qty: " + std::to_string(casada_no_nivel));
        }

        // Nivel esvaziou -> remove a entrada do mapa; o proximo begin() vira o topo.
        if (nivel->second.empty()) {
            oposto.erase(nivel);
        }
    }
}
