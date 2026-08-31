# Matching Engine

Uma *matching engine* (order matching system) para **um único ativo**, em memória, escrita em **C++17**. Cruza ordens `limit` e `market` respeitando prioridade **preço-tempo**, e expõe cancelamento, alteração e ordens *pegged*. Roda em dois modos que compartilham exatamente o mesmo núcleo: um **REPL** de terminal (stdin) e um **gateway ZMQ** ao vivo — e inclui um **dashboard interativo** em Streamlit para visualização em tempo real.

---

## Início rápido (TL;DR)

```bash
# 1. compilar (precisa de C++17 + CMake + libzmq/cppzmq)
cmake -S . -B build && cmake --build build

# 2. rodar os testes  ->  19/19 (núcleo) + 5/5 (journaling)
ctest --test-dir build --output-on-failure

# 3. ver o exemplo do enunciado rodando no núcleo
./build/mstly_engine repl < exemplos.txt
```

Para a demo visual ao vivo (order book + tape + gráfico de preço), veja **[Dashboard interativo](#dashboard-interativo-streamlit)** mais abaixo. Windows/MSYS2: veja a nota de `PATH` na seção de build.

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

### Dashboard interativo (Streamlit)
```bash
# Terminal 1 — engine
./build/mstly_engine zmq

# Terminal 2 — dashboard (abre no navegador em http://localhost:8501)
pip install streamlit plotly pyzmq   # dependências Python (uma vez)
streamlit run dashboard.py
```

O dashboard mostra **em tempo real**: gráfico de preço (last trade), profundidade do order book (bids/asks), tape de trades e suas ordens enviadas. Pela barra lateral você pode:

- **Gerador automático:** liga/desliga, controla velocidade (ordens/s), e alterna entre modo "só liquidez" (market maker — monta o book sem gerar trades) e modo livre (com market orders agressoras).
- **Ordem manual:** formulário limit/market ou campo de comando cru.

> **Modo "só liquidez":** o gerador ancora os preços das limits para **nunca cruzar o book** (buys < best ask, sells > best bid) e avança o mid **uma vez por tick** para impedir que ordens de um mesmo lote se cruzem entre si. O resultado: 0 trades vindos do gerador — quem fecha trades é você, pelos controles manuais.

### Gerador CLI (alternativo)
```bash
python market_feeder.py                 # 5 ordens/s, infinito (Ctrl+C p/ parar)
python market_feeder.py --rate 20       # mercado mais agitado
python market_feeder.py --duration 30   # roda 30s e para
python market_feeder.py --seed 42       # reproduzível
python market_feeder.py --only-liquidity  # só posta liquidez
python market_feeder.py --stop-on-exit  # ao sair, manda 'stop' e derruba a engine
```

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
                                 ┌─────────────────────────────────┐
                                 │    MatchingEngine (núcleo PURO) │
                                 │    cancel · modify · pegged     │
                                 └────────────▲────────────────────┘
                                              │ chama
[market_feeder PUSH] ─ordem (txt)─▶ Gateway (ZMQ)
[dashboard     PUSH]               PULL :5555     PUB :5556 ─eventos─▶ [dashboard SUB]
                                              │                         [feeder   SUB]
                                              │
                                   stdin REPL (mesma gramática)
```
- **Núcleo puro:** `MatchingEngine` não conhece `cout` nem sockets; reporta por um *sink* injetado. Isso a torna testável (o teste injeta um coletor) e permite que terminal, rede e journal sejam apenas *renderers* do mesmo fluxo de eventos.
- **Parser único:** `CommandProcessor` é a única gramática — o REPL e o gateway usam o mesmo caminho, então a rede se comporta igual ao terminal.
- **Gateway ZMQ:** `PULL tcp://*:5555` = *order-entry gateway* (entrada de ordens); `PUB tcp://*:5556` = *market-data feed* (broadcast dos eventos). Espelha a separação de uma exchange real entre canal de ordens e feed de dados. *(PUB/SUB tem "slow joiner": suba o listener antes do feeder.)*

### Simulador de mercado (`market_sim.py`)
O "cérebro" do gerador de ordens, compartilhado pelo `market_feeder.py` (CLI) e pelo `dashboard.py` (Streamlit). Implementa:
- **Random walk do preço justo (mid):** passos gaussianos com volatilidade configurável.
- **Geração de ordens limit passivas** em torno do mid, com spread aleatório em ticks.
- **Ordens market agressoras** (desligáveis pelo modo "só liquidez").
- **Cancelamento automático:** probabilístico (aleatório) + janela deslizante (cancela a mais velha quando >60 ordens vivas) para impedir acúmulo infinito no book.
- **Ancoragem anti-crossing:** em modo "só liquidez", os preços são ancorados no topo do book (`best_bid` / `best_ask`) para garantir que nenhuma limit do gerador cruze — o mid avança uma vez por tick (não por ordem), eliminando cruzamento intra-batch.

### Histórico em disco (journaling)
Todo o feed (comandos `IN`) e todos os eventos (`EV`) são gravados num `journal.tsv` append-only — formato TSV para cair direto no pandas (`pd.read_csv("journal.tsv", sep="\t", names=["kind","payload"])`). Duas implementações coexistem, mostrando a evolução de latência:

- **Fase A — síncrona (`Journal`, `engine/include/Journal.h`):** `record()` escreve direto num `std::ofstream` bufferizado. Simples e correto, mas o I/O de disco acontece **na mesma thread que casa ordens** — cada `record` pode bloquear o caminho quente.
- **Fase B — assíncrona (`AsyncJournal`, o default hoje):** tira o I/O do hot-path. O `record()` só faz um **`memcpy` numa arena em memória** (~O(1), zero syscall); uma **thread de drain** separada é quem grava em disco. O mecanismo é um **ping-pong de dois `ArenaAllocator`**: o produtor preenche a arena ativa; quando ela enche, entrega-a para a fila de drain e passa a preencher a outra; a thread grava a arena cheia **fora de qualquer lock**, faz `resetar()` O(1) e a devolve ao pool.
  - **Backpressure (a parte delicada):** se o produtor enche a arena ativa e a outra ainda não foi drenada (disco lento), o produtor **bloqueia até uma arena ficar livre**. É a escolha correta para um log de *auditoria*: preferimos uma pausa rara a **perder um evento** — as alternativas (descartar = perde auditoria; alocar arenas sem limite = risco de OOM) são piores. Dimensionando a arena para caber uma rajada típica (64 KiB por padrão), o bloqueio praticamente nunca ocorre.
  - **Invariante-chave:** o produtor **nunca** escreve no arquivo — só preenche arenas e as entrega. Toda escrita passa pela thread de drain, na ordem FIFO da fila ⇒ a ordem dos eventos no disco é idêntica à ordem em que chegaram. O teste `tests/test_journal.cpp` prova isso sob estresse (arenas minúsculas → milhares de rotações + backpressure; confere que nenhuma linha se perde e a ordem se mantém).
  - Está ligada nos **dois modos**: no REPL e no gateway ZMQ (que antes nem jornalizava).

---

## Testes
Dois alvos, registrados no CTest (`ctest --test-dir build`):
- `tests/test_core.cpp` — exercita o **núcleo** com um *sink* coletor, reproduzindo **os exemplos do enunciado linha a linha**: o matching principal (com agregação de trades e prioridade de chegada), o `print_book`, a limit marketable, a **alteração de ordem** (`modify` — mudança de preço perde prioridade) e a **ordem pegged** (acompanha o melhor bid e preserva o timestamp, ficando à frente de uma limit posterior no mesmo nível). Cada caso compara a saída esperada contra a obtida.
- `tests/test_journal.cpp` — exercita a **Fase B** (`AsyncJournal`) sob estresse: arenas minúsculas forçam milhares de rotações e backpressure, e o teste relê o arquivo para provar que **nenhum evento se perde e a ordem é preservada** (mais o caso de fallback da linha maior que a arena).

```
> ./test_core.exe && ./test_journal.exe
===== TESTES DO NUCLEO DA MATCHING ENGINE =====
===== RESUMO: 19/19 casos passaram =====
===== TESTES DA FASE B (AsyncJournal) =====
===== RESUMO: 5/5 casos passaram =====
```

## Limitações (escopo consciente)
**Um único ativo é especificação do enunciado**, não uma limitação — a arquitetura suportaria *multi-asset* com um book por símbolo, sem mudança estrutural. Cortes conscientes de fato, deliberados para o escopo do exercício: matching single-thread (evita locks no caminho quente), sem persistência perene além do journal, e sem checagem de risco/autenticação.

---

## Layout do repositório
```
engine/include/   DataStructure.h  MatchingEngine.h  CommandProcessor.h  Gateway.h
                  Journal.h (Fase A)  AsyncJournal.h (Fase B)  ArenaAllocator.h
engine/src/       MatchingEngine.cpp  CommandProcessor.cpp  Gateway.cpp
                  AsyncJournal.cpp  ArenaAllocator.cpp
src/              main.cpp          # ponto de entrada (repl | zmq)
tests/            test_core.cpp     test_journal.cpp
market_sim.py     # lógica de geração de ordens (random walk, ancoragem, cancelamento)
market_feeder.py  # gerador CLI de mercado (usa market_sim)
dashboard.py      # painel interativo Streamlit (usa market_sim)
exemplos.txt      # bateria de comandos p/ testar o REPL no seco
CMakeLists.txt  vcpkg.json
```
