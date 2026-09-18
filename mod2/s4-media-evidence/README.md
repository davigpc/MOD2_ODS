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
├── include/s4/                 # Headers públicos da biblioteca
│   ├── domain/                 # 1. Coração do negócio (zero dependências externas)
│   │   ├── entities/           # Entidades (MediaClip)
│   │   ├── value_objects/      # Objetos de Valor (TimeWindow, ClipDescriptor)
│   │   ├── repositories/       # Contratos de persistência (IMediaClipRepository)
│   │   ├── strategies/         # Padrão GoF Strategy de retenção (IRetentionStrategy)
│   │   └── errors/             # Hierarquia de exceções de domínio
│   ├── application/            # 2. Orquestração de casos de uso e DTOs
│   │   ├── use_cases/          # Casos de uso (ExtractClipUseCase, PurgeMediaUseCase)
│   │   └── dtos/               # Data Transfer Objects (ClipDescriptorDTO)
│   ├── infrastructure/         # 3. Adaptadores e integrações externas
│   │   ├── database/           # Implementações de repositório (SQLite, In-Memory)
│   │   ├── gstreamer/          # Fachada do buffer de RAM (IRAMBufferFacade)
│   │   └── b3_bus/             # Consumidor de eventos do Barramento B3
│   └── presentation/           # 4. Controladores e exposição de API
│       └── http/               # Controllers REST/gRPC (ClipDescriptorController)
├── src/                        # Implementações (.cpp)
│   ├── application/
│   │   └── use_cases/
│   ├── presentation/
│   │   └── http/
│   └── infrastructure/
└── tests/                      # Suíte de testes automatizados
    ├── unit/                   # Testes puros de domínio e aplicação (zero I/O)
    └── integration/            # Testes com filesystem e SQLite
```

---

## 🧩 Submódulos e Responsabilidades (Squad MOD-2)

Conforme definido em [`docs/responsability.md`](../../docs/responsability.md):

| Submódulo | Funcionalidade | Padrão / Arquitetura | Responsável |
| :--- | :--- | :--- | :--- |
| **S4.1 — Ring Buffer Contínuo** | Gravação circular em RAM (`/dev/shm`) dos últimos $N$ segundos pré-evento. | Facade + POSIX Shared Memory / GStreamer | Henrique Azevedo |
| **S4.2 — Binding Evento-Mídia** | Extração de trecho pré/pós evento da RAM, exportação em `.mp4` e hash SHA-256. | Command Handler + NVENC / Pipeline GStreamer | Davi Gomes |
| **S4.3 — Retenção & Expurgo LGPD** | Expurgo automático após 7 dias (LGPD) ou emergencial a 85% do NVMe (`is_locked_for_audit`). | Daemon Worker + Strategy Pattern (`RetentionStrategy`) | Henrique Azevedo |
| **S4.4 — API Descritores de Clipe** | Exposição de metadados e URIs locais sem trafegar vídeo binário pelo barramento. | REST / gRPC Controller | Davi Gomes |

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

### Executando Testes de Unidade

Os testes de unidade seguem a convenção `deve_[resultado]_quando_[condicao]` com zero dependências de I/O externo:

```bash
ctest --output-on-failure
# ou executar diretamente:
./test_s4_unit
```

---

## 📋 Checklist de Conformidade

Antes de submeter código ou PR:
- [x] **Clean Architecture**: Domínio não possui dependências de bibliotecas de terceiros ou frameworks.
- [x] **Sem Vídeo no Barramento**: Apenas metadados e DTOs trafegam nas APIs/mensageria.
- [x] **Nomenclatura Limpa**: Código autoexplicativo e funções focadas (SRP).
- [x] **Tratamento de Exceções**: Uso de exceções tipadas de `DomainError` e `ApplicationError`.
- [x] **Conformidade LGPD**: Entidade `MediaClip` possui controle de tempo de retenção e flag de trava de auditoria (`is_locked_for_audit`).
