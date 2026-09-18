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
├── third_party/httplib/        # cpp-httplib vendido (HTTP server)
├── include/s4/                 # Headers públicos da biblioteca
│   ├── domain/                 # 1. Coração do negócio (zero dependências externas)
│   │   ├── entities/           # Entidades (MediaClip)
│   │   ├── value_objects/      # Objetos de Valor (TimeWindow, ClipDescriptor)
│   │   ├── repositories/       # Contratos (IMediaClipRepository, IFileStorage)
│   │   ├── services/           # Porta de leitura do buffer (IMediaBufferReader)
│   │   ├── strategies/         # Padrão GoF Strategy de retenção (IRetentionStrategy)
│   │   └── errors/             # Hierarquia de exceções de domínio
│   ├── application/            # 2. Orquestração de casos de uso e DTOs
│   │   ├── use_cases/          # Casos de uso (ExtractClipUseCase, PurgeMediaUseCase futuro)
│   │   └── dtos/               # Data Transfer Objects (ClipDescriptorDTO, ISO 8601)
│   ├── infrastructure/         # 3. Adaptadores e integrações externas
│   │   ├── database/           # Implementações de repositório (SQLite, In-Memory)
│   │   ├── filesystem/         # Persistência em disco (FileStorage)
│   │   ├── hashing/            # Integridade criptográfica SHA-256 (OpenSSL)
│   │   ├── gstreamer/          # Fachada do buffer (IRAMBufferFacade + MockRAMBufferFacade)
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
| **S4.1 — Ring Buffer Contínuo** | Gravação circular em RAM (`/dev/shm`) dos últimos $N$ segundos pré-evento. **Mockado**: `MockRAMBufferFacade` (frames sintéticos) até integração real. | Facade + POSIX Shared Memory / GStreamer | Henrique Azevedo |
| **S4.2 — Binding Evento-Mídia** | Extração de trecho pré/pós evento da RAM, exportação em arquivo e hash SHA-256 real (OpenSSL). **Implementado** | Command Handler + SHA-256 | Davi Gomes |
| **S4.3 — Retenção & Expurgo LGPD** | Expurgo automático após 7 dias (LGPD) ou emergencial a 85% do NVMe (`is_locked_for_audit`). **Diferido** (somente contrato `IRetentionStrategy`). | Daemon Worker + Strategy Pattern | Henrique Azevedo |
| **S4.4 — API Descritores de Clipe** | Exposição de metadados e URIs locais sem trafegar vídeo binário. **Implementado**: REST `GET /api/v1/clips/{id}` | REST Controller + Clean Architecture | Davi Gomes |

---

## 📐 Padrões GoF Implementados

1. **Repository**: [`IMediaClipRepository`](include/s4/domain/repositories/media_clip_repository.hpp) isola completamente as regras de domínio do SQLite.
2. **Strategy**: [`IRetentionStrategy`](include/s4/domain/strategies/retention_strategy.hpp) permite alternar dinamicamente os algoritmos de expurgo (prazo LGPD vs. cota de disco FIFO).
3. **Facade**: [`IRAMBufferFacade`](include/s4/infrastructure/gstreamer/ram_buffer_facade.hpp) oculta a complexidade de ponteiros e pipes do GStreamer.
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

O daemon inicia a captura sintética (mock), extrai um clipe de exemplo e expõe a API:

```bash
curl http://localhost:8080/api/v1/clips/<clip_id>
sha256sum /tmp/s4-media/event-demo.mp4   # deve bater com "sha256_hash" do JSON
```

O plano de implementação e o backlog de integração real (S4.1/S4.3) estão em
[`docs/PLANO_IMPLEMENTACAO_S4.md`](../../docs/PLANO_IMPLEMENTACAO_S4.md).

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
