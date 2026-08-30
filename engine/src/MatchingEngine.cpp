#include "MatchingEngine.h"

#include <algorithm>  // std::min, std::max
#include <utility>    // std::move

// =============================================================================
//  Construtor
// =============================================================================
MatchingEngine::MatchingEngine(ReportSink sink, std::size_t pool_capacity)
    : report_sink(std::move(sink)) {
    // Pre-reserva memoria p/ evitar realocacoes do vector durante a operacao.
    // Como so' fazemos push_back em order_pool (nunca removemos do meio), os
    // indices (order_id) continuam validos p/ sempre — order_pool[id] e' estavel.
    order_pool.reserve(pool_capacity);
    order_locators.reserve(pool_capacity);
}

// =============================================================================
//  Reporter (unico ponto que "fala" com o mundo externo)
// =============================================================================
void MatchingEngine::reportar(const std::string& linha) const {
    if (report_sink) report_sink(linha);
}

// =============================================================================
//  Consumo de UM nivel de preco (FIFO / prioridade-tempo)
// =============================================================================
uint64_t MatchingEngine::consumir_nivel(NivelFila& fila, uint64_t& qty_agressora) {
    uint64_t casada = 0;

    // FIFO: sempre atacamos a ordem da FRENTE (a que chegou primeiro) ate ela
    // zerar; so' entao passamos p/ a proxima. Isso e' a "prioridade-tempo".
    while (qty_agressora > 0 && !fila.empty()) {
        const uint64_t id_passiva = fila.front();
        EngineOrder& passiva = order_pool[id_passiva];

        // Fill parcial permitido: casa o minimo entre o que a agressora ainda
        // quer e o que a passiva ainda tem.
        const uint64_t negociada = std::min(qty_agressora, passiva.qty);
        passiva.qty   -= negociada;
        qty_agressora -= negociada;
        casada        += negociada;

        if (passiva.qty == 0) {
            // Passiva totalmente preenchida: sai da fila do livro.
            fila.pop_front();
            // IMPORTANTE (fase 4): agora que cancel/modify LEEM os locators, uma
            // ordem preenchida precisa ter o locator invalidado — senao um cancel
            // posterior desse id usaria um iterador ja' invalidado (UB). O pop_front
            // acima invalida 'na_fila'; marcamos a ordem como morta.
            if (id_passiva < order_locators.size()) {
                order_locators[id_passiva].ativo = false;
            }
        }
        // Se sobrou qty na passiva, foi a agressora que zerou -> o while encerra
        // e a passiva continua na frente da fila (mantendo sua prioridade).
    }
    return casada;
}

// =============================================================================
//  descansar: a parte da limit que NAO casou vira liquidez passiva (FIFO/back)
// =============================================================================
uint64_t MatchingEngine::descansar(Side side, uint32_t price, uint64_t qty,
                                   int64_t id_forcado) {
    uint64_t id;
    if (id_forcado >= 0) {
        // Re-submissao do modify: MANTEM a identidade (id), mas recebe uma
        // prioridade-tempo NOVA (timestamp fresco) -> "perde prioridade".
        id = static_cast<uint64_t>(id_forcado);
        order_pool[id].qty       = qty;
        order_pool[id].price     = price;
        order_pool[id].side      = side;
        order_pool[id].type      = Type::LIMIT;
        order_pool[id].timestamp = next_seq++;
    } else {
        // Ordem nova: id de identidade + timestamp de prioridade (ambos avancam).
        id = next_order_id++;
        const uint64_t ts = next_seq++;
        order_pool.push_back(EngineOrder{
            /*order_id */ id,
            /*timestamp*/ ts,
            /*qty      */ qty,
            /*price    */ price,
            /*type     */ Type::LIMIT,
            /*side     */ side});
    }

    if (order_locators.size() <= id) order_locators.resize(id + 1);
    Locator& loc = order_locators[id];
    loc.side  = side;
    loc.ativo = true;

    // try_emplace: pega o nivel existente OU cria um vazio, devolvendo o iterador
    // do nivel (que guardamos no locator). push_back = FIM da fila = menor
    // prioridade-tempo. std::prev(end()) e' a posicao recem-inserida.
    if (side == Side::BUY) {
        auto nivel = bids.try_emplace(price).first;
        nivel->second.push_back(id);
        loc.na_fila   = std::prev(nivel->second.end());
        loc.nivel_bid = nivel;
    } else {
        auto nivel = asks.try_emplace(price).first;
        nivel->second.push_back(id);
        loc.na_fila   = std::prev(nivel->second.end());
        loc.nivel_ask = nivel;
    }
    return id;
}

// =============================================================================
//  inserir_por_timestamp: insere no MEIO da fila, na posicao por prioridade-tempo
//  (usado pela pegged, que preserva o timestamp original ao re-pegar)
// =============================================================================
void MatchingEngine::inserir_por_timestamp(uint64_t id, Side side, uint32_t price) {
    const uint64_t ts = order_pool[id].timestamp;

    if (order_locators.size() <= id) order_locators.resize(id + 1);
    Locator& loc = order_locators[id];
    loc.side  = side;
    loc.ativo = true;

    if (side == Side::BUY) {
        auto nivel = bids.try_emplace(price).first;
        NivelFila& fila = nivel->second;
        // Avanca enquanto as ordens a' frente forem MAIS ANTIGAS (ts menor); para
        // na primeira mais nova -> inserimos ANTES dela. Como ts e' unico e a
        // pegged preserva um ts anterior, ela entra a' frente das ordens novas.
        auto pos = fila.begin();
        while (pos != fila.end() && order_pool[*pos].timestamp < ts) ++pos;
        loc.na_fila   = fila.insert(pos, id);  // list::insert nao invalida os outros
        loc.nivel_bid = nivel;
    } else {
        auto nivel = asks.try_emplace(price).first;
        NivelFila& fila = nivel->second;
        auto pos = fila.begin();
        while (pos != fila.end() && order_pool[*pos].timestamp < ts) ++pos;
        loc.na_fila   = fila.insert(pos, id);
        loc.nivel_ask = nivel;
    }
}

// =============================================================================
//  desanexar_do_nivel: tira a ordem do nivel (O(1)); apaga o nivel se ficar vazio
// =============================================================================
void MatchingEngine::desanexar_do_nivel(uint64_t id) {
    Locator& loc = order_locators[id];
    if (loc.side == Side::BUY) {
        auto nivel = loc.nivel_bid;
        nivel->second.erase(loc.na_fila);            // O(1): remove o elemento
        if (nivel->second.empty()) bids.erase(nivel); // O(1): remove nivel vazio
    } else {
        auto nivel = loc.nivel_ask;
        nivel->second.erase(loc.na_fila);
        if (nivel->second.empty()) asks.erase(nivel);
    }
}

// =============================================================================
//  remover_do_livro: remocao DEFINITIVA (cancel / modify-perde-prioridade)
// =============================================================================
void MatchingEngine::remover_do_livro(uint64_t id) {
    desanexar_do_nivel(id);
    order_locators[id].ativo = false;
    order_pool[id].qty       = 0;  // defensivo: ordem morta nao tem qty viva
}

// =============================================================================
//  Miolo compartilhado: casa + descansa o resto (com id novo ou reutilizado)
// =============================================================================
int64_t MatchingEngine::processar_limit(int64_t id_forcado, Side side,
                                        uint32_t price, uint64_t qty) {
    if (side == Side::BUY) {
        // Compra casa contra ASKS cujo preco <= limite (melhor = menor primeiro).
        casar(asks, qty, [price](uint32_t preco_ask) { return preco_ask <= price; });
    } else {
        // Venda casa contra BIDS cujo preco >= limite (melhor = maior primeiro).
        casar(bids, qty, [price](uint32_t preco_bid) { return preco_bid >= price; });
    }

    if (qty == 0) {
        // Casou por inteiro: nada descansa. Se reutilizava um id (modify), a ordem
        // deixou de existir no livro -> marca morta.
        if (id_forcado >= 0) {
            order_locators[static_cast<uint64_t>(id_forcado)].ativo = false;
            order_pool[static_cast<uint64_t>(id_forcado)].qty = 0;
        }
        reavaliar_pegged();  // matching pode ter mexido no topo
        return -1;
    }

    // DECISAO "marketable-limit PREENCHE": o que sobrou depois de casar DESCANSA
    // no preco-limite (espelha exchange real — executa o que cruza e o resto vira
    // liquidez). A alternativa (rejeitar a ordem inteira) recusaria liquidez.
    const uint64_t id = descansar(side, price, qty, id_forcado);
    reavaliar_pegged();      // a nova liquidez / os trades podem ter mexido no topo
    return static_cast<int64_t>(id);
}

// =============================================================================
//  Ordem LIMITE (API publica)
// =============================================================================
int64_t MatchingEngine::on_limit_order(Side side, uint32_t price, uint64_t qty) {
    return processar_limit(/*id_forcado*/ -1, side, price, qty);
}

// =============================================================================
//  Ordem A MERCADO (casa contra qualquer preco; resto descartado)
// =============================================================================
void MatchingEngine::on_market_order(Side side, uint64_t qty) {
    // Sem limite de preco: o predicado sempre autoriza cruzar (retorna true).
    if (side == Side::BUY) {
        casar(asks, qty, [](uint32_t) { return true; });
    } else {
        casar(bids, qty, [](uint32_t) { return true; });
    }
    // O resto (qty > 0) e' DESCARTADO: market NUNCA descansa. 'qty' e' copia local.
    reavaliar_pegged();  // o consumo pode ter mudado o melhor bid/offer
}

// =============================================================================
//  FASE 4 — cancelamento
// =============================================================================
bool MatchingEngine::cancel(uint64_t id) {
    if (id >= order_locators.size() || !order_locators[id].ativo) {
        return false;  // id inexistente, ja' preenchido ou ja' cancelado
    }
    remover_do_livro(id);
    reavaliar_pegged();  // remover a ordem pode ter mudado o topo -> re-pega
    return true;
}

// =============================================================================
//  FASE 4 — alteracao de ordem
// =============================================================================
bool MatchingEngine::modify(uint64_t id,
                            std::optional<uint32_t> novo_preco,
                            std::optional<uint64_t> nova_qty) {
    if (id >= order_locators.size() || !order_locators[id].ativo) return false;

    EngineOrder& ord = order_pool[id];
    const uint32_t preco_final = novo_preco.value_or(ord.price);
    const uint64_t qty_final   = nova_qty.value_or(ord.qty);

    const bool mudou_preco  = (preco_final != ord.price);
    const bool aumentou_qty = (qty_final > ord.qty);

    // (a) SO' reducao (ou igual) de qty, MESMO preco -> in-place, MANTEM prioridade.
    // Justificativa: reduzir a quantidade nao da' vantagem a' ordem sobre quem ja'
    // estava atras dela na fila, entao nao ha razao p/ ela perder o lugar. Ja' um
    // AUMENTO de qty daria "fila furada" (pegaria mais liquidez mantendo o tempo
    // antigo), por isso cai na regra (b).
    if (!mudou_preco && !aumentou_qty) {
        if (qty_final == 0) {
            remover_do_livro(id);  // reduzir a zero equivale a cancelar
        } else {
            ord.qty = qty_final;   // decremento in-place; posicao na fila intacta
        }
        reavaliar_pegged();
        return true;
    }

    // (b) mudanca de PRECO ou AUMENTO de qty -> PERDE prioridade: remove e
    // re-submete como limit com o MESMO id. Ao re-submeter pode ficar marketable
    // e casar; o que sobrar descansa no fim da nova fila (timestamp fresco).
    const Side side = ord.side;
    remover_do_livro(id);
    processar_limit(static_cast<int64_t>(id), side, preco_final, qty_final);
    // processar_limit ja' chamou reavaliar_pegged().
    return true;
}

// =============================================================================
//  FASE 5 — ordem pegged
// =============================================================================
int64_t MatchingEngine::on_pegged_order(Side side, uint64_t qty, bool peg_to_bid) {
    // Aloca identidade + prioridade-tempo (a pegged tambem tem timestamp; e' o
    // que garante que, ao re-pegar, ela se re-ordene corretamente por tempo).
    const uint64_t id = next_order_id++;
    const uint64_t ts = next_seq++;
    order_pool.push_back(EngineOrder{
        /*order_id */ id,
        /*timestamp*/ ts,
        /*qty      */ qty,
        /*price    */ 0,          // preco real definido pela referencia abaixo
        /*type     */ Type::LIMIT,
        /*side     */ side});
    if (order_locators.size() <= id) order_locators.resize(id + 1);

    // Determina a referencia atual (melhor bid ou melhor offer).
    bool tem_ref = false;
    uint32_t ref = 0;
    if (peg_to_bid) {
        if (!bids.empty()) { ref = bids.begin()->first; tem_ref = true; }
    } else {
        if (!asks.empty()) { ref = asks.begin()->first; tem_ref = true; }
    }

    // Registra a pegged. Se ha' referencia, ja' a coloca no livro (por timestamp);
    // senao fica pendente e reavaliar_pegged() a colocara' quando surgir referencia.
    pegged_ativas.push_back(Pegged{id, peg_to_bid, /*colocada*/ tem_ref});
    if (tem_ref) {
        order_pool[id].price = ref;
        inserir_por_timestamp(id, side, ref);
    } else {
        order_locators[id].ativo = false;  // ainda nao esta' no livro
    }
    return static_cast<int64_t>(id);
}

// =============================================================================
//  mover_pegged: recoloca uma pegged VIVA num novo preco (preservando timestamp)
// =============================================================================
void MatchingEngine::mover_pegged(uint64_t id, uint32_t novo_preco) {
    desanexar_do_nivel(id);                 // sai do nivel atual (continua "viva")
    order_pool[id].price = novo_preco;      // atualiza o preco acompanhado
    inserir_por_timestamp(id, order_pool[id].side, novo_preco);  // re-entra por tempo
}

// =============================================================================
//  reavaliar_pegged: apos qualquer operacao, realinha as pegged a' referencia
// =============================================================================
void MatchingEngine::reavaliar_pegged() {
    if (pegged_ativas.empty()) return;

    // Ponto-fixo: mover uma pegged pode mudar o topo e afetar outra. A guarda de
    // idempotencia (so' move se o preco DIFERIR) garante convergencia; o contador
    // 'guarda' e' so' um cinto de seguranca contra qualquer ciclo inesperado.
    bool mudou = true;
    int guarda = 0;
    while (mudou && guarda++ < 1000) {
        mudou = false;
        for (auto it = pegged_ativas.begin(); it != pegged_ativas.end();) {
            const uint64_t id = it->id;

            // Pegged que ja' foi colocada mas morreu (preenchida/cancelada) sai do
            // registro. (Uma ainda-nao-colocada permanece esperando referencia.)
            if (it->colocada && !order_locators[id].ativo) {
                it = pegged_ativas.erase(it);
                continue;
            }

            // Referencia atual. IMPORTANTE: usamos o melhor bid/offer do livro
            // INCLUINDO a propria pegged. Logo, se a pegged JA' e' o topo, a
            // referencia = seu proprio preco -> estavel, nao se move (evita loop e
            // segue a instrucao do enunciado). Limitacao consciente: uma pegged
            // no topo nao "desce" sozinha se o resto do mercado recuar; p/ isso
            // usariamos o melhor preco EXCLUINDO a pegged. Aqui priorizamos bater
            // o exemplo do e-mail e a simplicidade.
            bool tem_ref = false;
            uint32_t ref = 0;
            if (it->peg_to_bid) {
                if (!bids.empty()) { ref = bids.begin()->first; tem_ref = true; }
            } else {
                if (!asks.empty()) { ref = asks.begin()->first; tem_ref = true; }
            }
            if (!tem_ref) { ++it; continue; }  // ainda sem referencia: espera

            if (!it->colocada) {
                // Havia uma pendente e agora ha' referencia: coloca-a.
                order_pool[id].price = ref;
                inserir_por_timestamp(id, order_pool[id].side, ref);
                it->colocada = true;
                mudou = true;
            } else if (ref != order_pool[id].price) {
                // A referencia mudou: acompanha (recolocada por timestamp).
                mover_pegged(id, ref);
                mudou = true;
            }
            ++it;
        }
    }
}

// =============================================================================
//  Consultas
// =============================================================================
uint64_t MatchingEngine::qty_of(uint64_t id) const {
    return (id < order_pool.size()) ? order_pool[id].qty : 0;
}

bool MatchingEngine::is_live(uint64_t id) const {
    return id < order_locators.size() && order_locators[id].ativo;
}

// =============================================================================
//  format_price — centavos (uint32_t) -> string sem zeros inuteis
// =============================================================================
std::string MatchingEngine::format_price(uint32_t centavos_de_preco) {
    const uint32_t parte_inteira = centavos_de_preco / 100;   // reais
    const uint32_t centavos      = centavos_de_preco % 100;   // 0..99

    // Preco "redondo" (ex.: 1000): sem parte decimal.
    if (centavos == 0) {
        return std::to_string(parte_inteira);
    }

    std::string resultado = std::to_string(parte_inteira) + ".";

    if (centavos % 10 == 0) {
        // Ex.: 1050 -> 50 -> "5" (uma casa; remove o zero final inutil).
        resultado += std::to_string(centavos / 10);
    } else {
        // Ex.: 999 -> 99 -> "99";  1001 -> 1 -> "01" (zero a esquerda importa).
        if (centavos < 10) resultado += '0';
        resultado += std::to_string(centavos);
    }
    return resultado;
}

// =============================================================================
//  print_book — uma linha por ORDEM (nao agregado). Formato do e-mail.
// =============================================================================
void MatchingEngine::print_book() const {
    // Coluna esquerda = bids (maior preco primeiro, FIFO dentro do nivel).
    // Coluna direita  = asks (menor preco primeiro). Ambos ja iteram na ordem
    // certa gracas aos comparadores dos mapas.
    std::vector<std::string> coluna_bids;
    for (const auto& [preco, fila] : bids)
        for (uint64_t id : fila)
            coluna_bids.push_back(std::to_string(order_pool[id].qty) + " @ " +
                                  format_price(preco));

    std::vector<std::string> coluna_asks;
    for (const auto& [preco, fila] : asks)
        for (uint64_t id : fila)
            coluna_asks.push_back(std::to_string(order_pool[id].qty) + " @ " +
                                  format_price(preco));

    // Largura da coluna esquerda: acomoda o cabecalho ("Ordens de Compra" = 16)
    // e a maior linha de bid, com 1 espaco de folga antes da barra. Usar a mesma
    // largura no cabecalho, no separador e nas linhas garante o alinhamento.
    const std::string titulo_esq = "Ordens de Compra";
    std::size_t largura = titulo_esq.size();
    for (const auto& s : coluna_bids) largura = std::max(largura, s.size());
    largura += 1;

    // Helper local: pega uma string, completa com espacos ate 'largura' e cola a
    // barra. Se houver conteudo a' direita, adiciona " " + conteudo.
    auto monta_linha = [&](std::string esquerda, const std::string& direita) {
        esquerda.resize(largura, ' ');
        std::string linha = esquerda + "|";
        if (!direita.empty()) linha += " " + direita;
        return linha;
    };

    reportar(monta_linha(titulo_esq, "Ordens de Venda"));
    reportar(std::string(largura, '-') + "|" + std::string(largura, '-'));

    const std::size_t linhas = std::max(coluna_bids.size(), coluna_asks.size());
    for (std::size_t i = 0; i < linhas; ++i) {
        const std::string esq = (i < coluna_bids.size()) ? coluna_bids[i] : "";
        const std::string dir = (i < coluna_asks.size()) ? coluna_asks[i] : "";
        reportar(monta_linha(esq, dir));
    }
}
