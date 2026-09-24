# S4 — Evidência de Mídia (Media Evidence)

> Componente da **Camada 3 (Serviço)** do Squad **MOD-2** (Mídia, Métricas e Interfaces) no projeto **ODS 2026/2**.

O serviço **S4** é responsável por gerenciar o ciclo de vida das evidências audiovisuais na borda (**NVIDIA Jetson Orin Nano**), incluindo captura contínua em buffer de memória RAM, extração de clipes probatórios vinculados a eventos do Barramento B3, persistência em SSD NVMe com integridade criptográfica (SHA-256) e expurgo automático conforme LGPD e políticas de cota de disco.

---

## 🏗️ Arquitetura e Estrutura de Diretórios

O projeto adota os princípios de **Clean Architecture**, **Domain-Driven Design (DDD)** e **Design Patterns (GoF)**, desenvolvido em **C++20** com sistema de build **CMake**.

```text
mod2/s4-media-evidence/
├── CMakeLists.txt              # Configuração de build C++20 e dependências
├── README.md                   # Documentação do componente
├── Dockerfile                  # Imagem OCI multi-arch (builder + runtime)
├── docker-compose.yml          # Orquestração local (porta 8080 + volume)
├── .dockerignore               # Exclusões do contexto de build
├── third_party/httplib/        # cpp-httplib vendido (HTTP server)
├── include/s4/                 # Headers públicos da biblioteca
│   ├── domain/                 # 1. Coração do negócio (zero dependências externas)
│   │   ├── entities/           # Entidades (MediaClip, RingBuffer)
│   │   ├── value_objects/      # Objetos de Valor (TimeWindow, ClipDescriptor,
│   │   │                       #   CaptureWindow, FrameDescriptor, BufferCapacity)
│   │   ├── repositories/       # Contratos (IMediaClipRepository, IFileStorage, IFrameStore)
│   │   ├── services/           # Portas do buffer (IMediaBufferReader, IFrameSource)
│   │   ├── strategies/         # Padrão GoF Strategy de retenção (IRetentionStrategy)
│   │   └── errors/             # Hierarquia de exceções de domínio
│   ├── application/            # 2. Orquestração de casos de uso e DTOs
│   │   ├── use_cases/          # Casos de uso (ExtractClipUseCase, PurgeMediaUseCase futuro)
│   │   └── dtos/               # Data Transfer Objects (ClipDescriptorDTO, ISO 8601)
│   ├── infrastructure/         # 3. Adaptadores e integrações externas
│   │   ├── database/           # Implementações de repositório (SQLite, In-Memory)
│   │   ├── filesystem/         # Persistência em disco (FileStorage)
│   │   ├── hashing/            # Integridade criptográfica SHA-256 (OpenSSL)
│   │   ├── gstreamer/          # Fachada do buffer: contrato, mock, RingBufferRAMFacade
│   │   │                       #   e a fonte de captura (appsink_frame_source)
│   │   ├── ringbuffer/         # S4.1: arena /dev/shm, ponte de relógios,
│   │   │                       #   fonte sintética e cenário gravado
│   │   └── b3_bus/             # Consumidor de eventos do Barramento B3 (contrato)
│   └── presentation/           # 4. Controladores e exposição de API
│       └── http/               # Controller + servidor REST (ClipDescriptorHttpServer)
├── src/                        # Implementações (.cpp)
│   ├── application/use_cases/  # extract_clip.cpp
│   ├── presentation/http/      # controller + servidor REST
│   └── infrastructure/         # hashing, database, filesystem, gstreamer
├── main.cpp → src/main.cpp     # Daemon demo executável
└── tests/                      # Suíte de testes automatizados
    ├── unit/                   # Testes puros (domínio, casamento de uso, hash)
    └── integration/            # Testes com SQLite, filesystem real e HTTP
```

---

## 🧩 Submódulos e Responsabilidades (Squad MOD-2)

Conforme definido em [`docs/responsability.md`](../../docs/responsability.md):

| Submódulo | Funcionalidade | Padrão / Arquitetura | Responsável |
| :--- | :--- | :--- | :--- |
| **S4.1 — Ring Buffer Contínuo** | Gravação circular em RAM (`/dev/shm`) dos últimos $N$ segundos pré-evento, com alinhamento a keyframe e espera do pós-evento. **Implementado**: `RingBufferRAMFacade` (substitui o `MockRAMBufferFacade`, que permanece como dublê de teste). Ver [`docs/S4.1-ring-buffer.md`](docs/S4.1-ring-buffer.md). | Facade + POSIX Shared Memory / GStreamer | Henrique Azevedo |
| **S4.2 — Binding Evento-Mídia** | Extração de trecho pré/pós evento da RAM, exportação em arquivo e hash SHA-256 real (OpenSSL). **Implementado** | Command Handler + SHA-256 | Davi Gomes |
| **S4.3 — Retenção & Expurgo LGPD** | Expurgo automático após 7 dias (LGPD) ou emergencial a 85% do NVMe (`is_locked_for_audit`). **Diferido** (somente contrato `IRetentionStrategy`). | Daemon Worker + Strategy Pattern | Henrique Azevedo |
| **S4.4 — API Descritores de Clipe** | Exposição de metadados e URIs locais sem trafegar vídeo binário. **Implementado**: REST `GET /api/v1/clips/{id}` + `POST /api/v1/events` (simulação de novo evento em runtime) | REST Controller + Clean Architecture | Davi Gomes |

---

## 🎞️ S4.1 — Ring Buffer Contínuo: entrada → processamento → saída

Resumo do contrato do S4.1. O detalhamento completo (funcionamento interno, decisões de projeto, dimensionamento)
está em [`docs/S4.1-ring-buffer.md`](docs/S4.1-ring-buffer.md).

### O que recebe

| Entrada | De onde vem | Formato |
| :--- | :--- | :--- |
| **Quadros de vídeo** | Câmera (P4) via GStreamer `appsink`, replay gravado (P6) ou fonte sintética (demo/testes) | `CapturedFrame { captureTsNs, data, length, isKeyframe, sessionId }` — H.264 Annex-B, 1 quadro por chamada |
| **Pedido de trecho** | S4.2 (`ExtractClipUseCase`), disparado pelo `POST /api/v1/events` ou pelo barramento B3 | `readWindow(start, end)` em relógio de parede, ou `extractAround(tsEvento, preSeg, posSeg, timeout)` em relógio de captura |
| **Configuração** | Build da aplicação | `RingBufferConfig { cameraId, windowSeconds, bitrateBps, safetyFactor, width, height, replaceStaleSegment }` |

### O que faz

1. **Aloca uma arena fixa em RAM compartilhada** (`/dev/shm/ods_s4_ring_<cameraId>`), dimensionada por
   `bitrate / 8 × windowSeconds × safetyFactor`. Exemplo: 1080p a 4 Mbps com 30 s ≈ 21 MB por câmera. Nada é escrito no NVMe.
2. **Grava continuamente** cada quadro na posição do ponteiro de escrita. Quando a arena enche, o ponteiro volta ao início
   e **os quadros mais antigos são expulsos**, então o buffer guarda sempre os últimos *N* segundos.
3. **Mantém um índice** (`FrameDescriptor { sequence, captureTsNs, offset, length, isKeyframe, sessionId }`), ou seja,
   os "ponteiros de frames" da especificação.
4. **Protege a linha do tempo**: rejeita e conta quadros com relógio retrocedendo e reinicia o buffer quando o `sessionId`
   muda (replay/reinício), para nunca misturar duas sessões num mesmo clipe.
5. **Responde a pedidos de trecho**: devolve os quadros da janela **começando no keyframe anterior** (senão o trecho não
   seria decodificável) e, se o pós-evento ainda não foi capturado, **espera por ele** até o timeout.

### O que entrega

| Saída | Para quem | Conteúdo |
| :--- | :--- | :--- |
| `std::vector<std::vector<uint8_t>>` (`readWindow`) | S4.2, via porta `IMediaBufferReader` | Payloads H.264 da janela, começando em keyframe. **Lista vazia** se a janela não está no buffer ou se a captura já foi parada (o S4.2 converte em `MediaBufferEmptyError` → HTTP 500). |
| `std::vector<VideoFrame>` (`extractWindow`) | S4.2, via `IRAMBufferFacade` | Os mesmos quadros, com timestamp de parede e dimensões |
| `ExtractedSegment` (`extractAround`) | Quem conhece o relógio de captura | Quadros + `sessionId` + flags `isTruncatedAtStart` / `isTruncatedAtEnd` |
| `BufferStats` (`stats()`) | Observabilidade | Quadros guardados, segundos cobertos, bytes usados, total ingerido/expulso/rejeitado |
| Segmento `/dev/shm/ods_s4_ring_<cameraId>` | Outros processos (leitura sem cópia) | Os bytes brutos da arena circular |

### Integração com o S4.2 / S4.4 (fluxo de um evento)

```text
POST /api/v1/events {"event_id": "evt-1"}              (S4.4, Davi)
  └─> ExtractClipUseCase.execute(evt-1, [agora-1s, agora])   (S4.2, Davi)
        └─> RingBufferRAMFacade.readWindow(start, end)      (S4.1)  → quadros H.264 da RAM
        └─> grava $media_dir/evt-1.mp4 + SHA-256 + SQLite
  <── 201 {clip_id, file_uri, start_time, end_time, sha256_hash, ...}
```

O `main.cpp` liga as duas partes: o `RingBufferRAMFacade` é injetado no `ExtractClipUseCase` como `IMediaBufferReader`.
O `MockRAMBufferFacade` continua no repositório apenas como dublê dos testes do S4.2.

### Validação (24/09/2026, imagem Docker `debian:bookworm-slim`, Release)

| Verificação | Resultado |
| :--- | :--- |
| `ctest` (8 suítes, incluindo `test_s4_ring_buffer` e `test_s4_ring_buffer_runtime`) | 8/8 passaram |
| ThreadSanitizer nas suítes do S4.1 | nenhuma condição de corrida |
| Daemon em container: `POST /api/v1/events` | `201` com DTO; SHA-256 do arquivo igual ao do JSON; `400` para `event_id`/janela inválidos; `500` para janela fora do buffer |
| Buffer após mais de 30 s (arena já deu a volta) | extração continua funcionando; segmento `/dev/shm` com tamanho fixo (756 000 B no demo) |
| `docker stop` com eventos chegando | encerramento limpo (exit 0), sem acesso à arena já desmapeada |

---

## 📐 Padrões GoF Implementados

1. **Repository**: [`IMediaClipRepository`](include/s4/domain/repositories/media_clip_repository.hpp) isola completamente as regras de domínio do SQLite.
2. **Strategy**: [`IRetentionStrategy`](include/s4/domain/strategies/retention_strategy.hpp) permite alternar dinamicamente os algoritmos de expurgo (prazo LGPD vs. cota de disco FIFO).
3. **Facade**: [`IRAMBufferFacade`](include/s4/infrastructure/gstreamer/ram_buffer_facade.hpp) oculta a complexidade de ponteiros e pipes do GStreamer — implementado por [`RingBufferRAMFacade`](include/s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp) (produção) e `MockRAMBufferFacade` (testes).
4. **Observer / Consumer**: [`B3EventConsumer`](include/s4/infrastructure/b3_bus/b3_event_consumer.hpp) assina eventos do barramento B3 e dispara os casos de uso.

---

## ⚙️ Pré-requisitos e Dependências

- **Compilador C++**: GCC 11+, Clang 13+ ou compatível com **C++20**
- **CMake**: Versão 3.20 ou superior
- **Bibliotecas do Sistema**:
  - `sqlite3` (persistência de metadados)
  - `OpenSSL` (cálculo de hash SHA-256)
  - `GStreamer 1.0` (captura e encode de vídeo)

---

## 🔨 Compilação e Testes

### Compilando o Projeto

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

### Executando Testes

Os testes de unidade seguem a convenção `deve_[resultado]_quando_[condicao]`. Testes de unidade usam zero I/O
(exceto o cálculo de hash); a integração cobre SQLite em arquivo temporário, extração com filesystem real e servidor HTTP.

```bash
ctest --output-on-failure
```

### Executando o Serviço (daemon demo)

```bash
./s4_media_evidence --port 8080 --db /tmp/s4.db --media-dir /tmp/s4-media
```

O daemon inicia o ring buffer real do S4.1 em `/dev/shm/ods_s4_ring_cam0` (alimentado por uma fonte sintética,
já que a Jetson e a câmera não estão presentes em toda máquina de desenvolvimento), extrai um clipe de exemplo e expõe a API:

```bash
curl http://localhost:8080/api/v1/clips/<clip_id>
sha256sum /tmp/s4-media/event-demo.mp4   # deve bater com "sha256_hash" do JSON
ls -lh /dev/shm/ods_s4_ring_cam0         # o buffer circular, enquanto o daemon roda
```

### Simulando uma nova entrada de evento

Para injetar um evento em runtime (sem reiniciar o daemon), use `POST /api/v1/events`. Cada chamada extrai um novo
clipe do buffer, persiste em `$media_dir/<event_id>.mp4` e responde `201` com o DTO do clipe:

```bash
curl -X POST http://127.0.0.1:8080/api/v1/events \
  -H 'Content-Type: application/json' \
  -d '{"event_id": "evt-2026-0001"}'

# janela explícita em epoch milliseconds (opcional; default = últimos 1s)
curl -X POST http://127.0.0.1:8080/api/v1/events \
  -H 'Content-Type: application/json' \
  -d '{"event_id": "evt-2026-0002", "start_ms": 1720000000000, "end_ms": 1720000001000}'
```

- `event_id` ausente → gerado automaticamente (`event-<uuid>`); deve conter apenas `[A-Za-z0-9_-]`.
- Janela inválida (`start_ms >= end_ms`) → `400`; janela sem frames no buffer → `500`.

> **Nota de build:** o modo `Release` define `NDEBUG` e remove todos os `assert()`. Os testes do S4.1 usam
> `ODS_CHECK` ([`tests/ods_check.hpp`](tests/ods_check.hpp)), que vale em qualquer modo de build.

O plano de implementação e o backlog de integração real (S4.1/S4.3) estão em
[`docs/PLANO_IMPLEMENTACAO_S4.md`](../../docs/PLANO_IMPLEMENTACAO_S4.md).

---

## 🐳 Containerização (Docker)

Imagem OCI **multi-arch** (`debian:bookworm-slim`, multi-stage): o estágio `builder` compila em C++20 e executa o
`ctest` completo (falha de teste quebra o build), e o estágio `runtime` final contém apenas as bibliotecas em tempo de
execução e o binário, rodando como usuário não-root `s4` com `VOLUME /data`.

```bash
# Build (testes rodam dentro do builder)
docker build -t ods/s4-media-evidence:0.1.0 ./mod2/s4-media-evidence

# Executar (dados persistidos em volume nomeado)
docker run -d --rm --name s4 -p 8080:8080 -v s4_data:/data ods/s4-media-evidence:0.1.0
curl http://127.0.0.1:8080/healthz                # {"status": "ok"}

# Ou via docker compose (healthcheck + porta + volume)
docker compose -f mod2/s4-media-evidence/docker-compose.yml up -d --build
```

Detalhes, verificação (smoke test) e instruções para a **Jetson Orin Nano (arm64)** — build nativo na própria Jetson
ou alternativa com `buildx`/`binfmt` — estão em
[`docs/PLANO_CONTAINERIZACAO_S4.md`](../../docs/PLANO_CONTAINERIZACAO_S4.md).

---

## 📋 Checklist de Conformidade

Antes de submeter código ou PR:
- [x] **Clean Architecture**: Domínio não possui dependências de bibliotecas de terceiros ou frameworks.
- [x] **Sem Vídeo no Barramento**: Apenas metadados e DTOs trafegam nas APIs/mensageria.
- [x] **Nomenclatura Limpa**: Código autoexplicativo e funções focadas (SRP).
- [x] **Tratamento de Exceções**: Uso de exceções tipadas de `DomainError` e `ApplicationError`.
- [x] **Conformidade LGPD**: Entidade `MediaClip` possui controle de tempo de retenção e flag de trava de auditoria (`is_locked_for_audit`).
- [x] **Testes de Unidade**: Suíte `deve_..._quando_...` passando (unit + integration).
- [ ] **S4.3 — Expurgo LGPD**: pendente (contrato `IRetentionStrategy`; implementação real no backlog da Henrique).
