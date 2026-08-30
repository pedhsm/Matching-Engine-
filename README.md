# Matching Engine

Uma *matching engine* (order matching system) para **um único ativo**, em memória, escrita em **C++17**. Cruza ordens `limit` e `market` respeitando prioridade **preço-tempo**, e expõe cancelamento, alteração e ordens *pegged*. Roda em dois modos que compartilham exatamente o mesmo núcleo: um **REPL** de terminal (stdin) e um **gateway ZMQ** ao vivo.

---

## Como compilar e rodar

**Requisitos:** compilador C++17, CMake, e `libzmq` + `cppzmq`.

### Linux / macOS
```bash
# libs (ex.: Debian/Ubuntu)  sudo apt install libzmq3-dev
#         (macOS/Homebrew)   brew install zeromq cppzmq
cmake -S . -B build
cmake --build build
```

### Windows (máquina de desenvolvimento — MSYS2 UCRT64)
```powershell
# libs: pacman -S mingw-w64-ucrt-x86_64-zeromq mingw-w64-ucrt-x86_64-cppzmq
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"
cmake --build build
```

### Executar
```bash
./build/mstly_engine repl     # modo terminal (lê comandos do stdin)   [default]
./build/mstly_engine zmq      # modo ao vivo (gateway ZMQ)
```
No modo `repl` você pode alimentar um arquivo de comandos: `./build/mstly_engine repl < exemplos.txt`.

---

## Comandos (mesma gramática no REPL e no ZMQ)

```
limit  <buy|sell> <preço> <qty>
market <buy|sell> <qty>
cancel order <id>
modify order <id> price <preço>
modify order <id> qty <qty>
modify order <id> <preço> <qty>
peg bid   <buy|sell> <qty>      # acompanha o melhor bid
peg offer <buy|sell> <qty>      # acompanha o melhor offer
print book                       # (também: print_book | book)
```

Quando um trade acontece, a saída é `Trade, price: <preço>, qty: <qty>`. Ao criar uma limit que descansa: `Order created: <side> <qty> @ <preço> <id>`. O `<id>` retornado é usado no `cancel`/`modify`.

Reproduzindo o exemplo do enunciado:
```
> limit buy 10 100
> limit sell 20 100
> limit sell 20 200
> market buy 150
Trade, price: 20, qty: 150
> market buy 200
Trade, price: 20, qty: 150
> market sell 200
Trade, price: 10, qty: 100
```

---

## Decisões de design

### Estruturas de dados
O livro é mantido em **dois `std::map`** de preço → fila: `asks` em ordem crescente e `bids` em ordem decrescente (`std::greater`). Cada nível de preço guarda uma **`std::list`** de ordens (FIFO). Escolhas e porquês:

| Estrutura | Papel | Por quê |
|-----------|-------|---------|
| `std::map` | níveis de preço | árvore balanceada: insert/remove em `O(log M)` (M = níveis distintos, **não** ordens); `begin()` = melhor preço em `O(1)` (leftmost cacheado). |
| `std::list` por nível | fila FIFO (prioridade-tempo) | iteradores **estáveis**: apagar um nó não invalida os outros — é o que habilita o cancel `O(1)`. |
| `order_locators` | id → (iterador da list + do nível) | `cancel`/`modify` acham a ordem sem varrer o book. |
| `order_pool` | dados das ordens pré-alocados | zero `malloc` no caminho quente = latência previsível. |
| preço em `uint32_t` (centavos) | representação de preço | exchanges usam ticks inteiros; elimina erro de ponto flutuante nas comparações. A formatação (`9.99`, `10.5`) é só na saída. |

### Complexidade — e a exigência de O(N)
Notação: **N** = ordens vivas · **M** = níveis distintos (na prática **M ≪ N**) · **K** = fills de uma ordem.

| Operação | Custo |
|----------|-------|
| Melhor preço (best bid/ask) | `O(1)` |
| Cancel / modify-qty-reduz | `O(1)` |
| Inserir limit / modify de preço | `O(log M)` |
| Matching (market/marketable) | `O(K)` (proporcional aos fills) |
| `print book` | `O(N)` (proporcional à saída) |

O enunciado pede `O(N)` — que é um teto **fraco**. O anti-padrão a evitar é a engine ingênua que **varre o book** a cada operação (`O(N)` por op → `O(N²)` total). Este design é `O(1)`/`O(log M)` por operação — **abaixo do pedido** — e `log M` é praticamente constante (M são níveis, tipicamente centenas). Os custos `O(K)` e `O(N)` restantes são **output-proporcionais**: para gerar K trades é preciso tocar K ordens, e isso nenhum algoritmo evita. *(Se fosse necessário `O(1)` puro no insert, a rota seria trocar o `std::map` por um array indexado por tick + bitmap de níveis ocupados — mais memória e faixa de preço fixa; escolha deliberada de não fazer.)*

### Semânticas escolhidas (e justificadas)
- **Limit marketable preenche.** Uma limit cujo preço cruzaria o book executa a parte que cruza e o restante descansa no preço-limite. Espelha uma ordem agressiva de exchange real; ignorar a ordem recusaria liquidez legítima. *(O enunciado permite qualquer um dos dois comportamentos desde que justificado.)*
- **Market descarta o não-preenchido.** Uma market nunca vira liquidez passiva; o que não casa é descartado.
- **Trade agregado por nível de preço.** Consumir 100 + 50 no mesmo preço 20 emite **uma** linha (`qty: 150`), como no exemplo do enunciado.
- **Modify e prioridade.** Redução de qty mantém a prioridade (in-place); **aumento de qty ou mudança de preço perde a prioridade**, recolocando a ordem no fim da fila do nível correspondente.
- **Pegged preserva o timestamp.** Uma ordem *pegged* acompanha o melhor bid/offer e, ao ser recolocada num novo nível, mantém seu timestamp original — por isso ela pode ficar à frente de uma ordem que chegou depois no mesmo preço, sem violar a prioridade-tempo.

### Arquitetura
```
[feeder PUSH] ──ordem (texto)──▶ Gateway (ZMQ)  ──chama──▶ MatchingEngine (núcleo PURO)
[listener SUB] ◀──eventos────── PUB :5556           ▲
                                                     └── stdin REPL (mesma gramática)
```
- **Núcleo puro:** `MatchingEngine` não conhece `cout` nem sockets; reporta por um *sink* injetado. Isso a torna testável (o teste injeta um coletor) e permite que terminal, rede e journal sejam apenas *renderers* do mesmo fluxo de eventos.
- **Parser único:** `CommandProcessor` é a única gramática — o REPL e o gateway usam o mesmo caminho, então a rede se comporta igual ao terminal.
- **Gateway ZMQ:** `PULL tcp://*:5555` = *order-entry gateway* (entrada de ordens); `PUB tcp://*:5556` = *market-data feed* (broadcast dos eventos). Espelha a separação de uma exchange real entre canal de ordens e feed de dados. *(PUB/SUB tem "slow joiner": suba o listener antes do feeder.)*

### Histórico em disco (journaling)
- **Fase A (implementada):** todo o feed e todos os eventos são gravados em `journal.tsv` via `std::ofstream` bufferizado — consulta posterior no pandas (`pd.read_csv("journal.tsv", sep="\t")`). No caminho síncrono, o próprio `ofstream` já batela as escritas em blocos.
- **Fase B (projetada):** para não bloquear o caminho quente no I/O, o design previsto é um **ping-pong de dois `ArenaAllocator`** + uma **thread de drain**: o hot-path preenche a arena A; ao encher, entrega A para a thread gravar em disco enquanto já preenche a B; troca e reseta. É onde a arena se justifica (o `ofstream` síncrono já basta na Fase A). O tratamento de *backpressure* (o que fazer se o drain não terminou antes de a segunda arena encher) é a parte delicada. O esqueleto do `ArenaAllocator` está em `engine/include/ArenaAllocator.h`.

---

## Testes
`tests/test_core.cpp` exercita o núcleo com um *sink* coletor, reproduzindo o exemplo do enunciado (matching), o `print_book`, e a limit marketable, com verificação linha a linha.

## Limitações (escopo consciente)
Um único ativo; matching single-thread; sem persistência perene além do journal; sem checagem de risco/autenticação; a Fase B do journaling está documentada, não implementada. São cortes deliberados para o escopo do exercício.

---

## Layout do repositório
```
engine/include/   DataStructure.h  MatchingEngine.h  CommandProcessor.h  Gateway.h  Journal.h  ArenaAllocator.h
engine/src/       MatchingEngine.cpp  CommandProcessor.cpp  Gateway.cpp  ArenaAllocator.cpp
src/              main.cpp        # ponto de entrada (repl | zmq)
tests/            test_core.cpp
feeder.py         # cliente PUSH de exemplo para o modo ZMQ
CMakeLists.txt  vcpkg.json
```
