#include <iostream>
#include <memory>
#include <cassert>
#include "s4/domain/entities/media_clip.hpp"
#include "s4/domain/value_objects/time_window.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/presentation/http/clip_descriptor_controller.hpp"

using namespace ods::s4;

void test_deve_calcular_duracao_corretamente_quando_janela_for_valida() {
    auto now = std::chrono::system_clock::now();
    auto later = now + std::chrono::seconds(30);
    domain::TimeWindow tw(now, later);
    assert(tw.durationSeconds() >= 29.9 && tw.durationSeconds() <= 30.1);
}

void test_deve_lancar_excecao_quando_start_time_for_maior_ou_igual_ao_end_time() {
    auto now = std::chrono::system_clock::now();
    auto earlier = now - std::chrono::seconds(1);
    bool threw = false;
    try {
        domain::TimeWindow tw(now, earlier);
    } catch (const domain::InvalidTimeWindowError&) {
        threw = true;
    }
    assert(threw);
}

void test_deve_extrair_clipe_e_consultar_via_controller() {
    auto repo = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    application::ExtractClipUseCase useCase(repo);

    auto start = std::chrono::system_clock::now();
    auto end = start + std::chrono::seconds(15);
    domain::TimeWindow tw(start, end);

    auto clip = useCase.execute("evt-123", tw, "/nvme/clips/evt-123.mp4");
    assert(clip.eventId() == "evt-123");
    assert(clip.isRetained());

    presentation::ClipDescriptorController controller(repo);
    auto desc = controller.getClipDescriptor(clip.clipId());
    assert(desc.has_value());
    assert(desc->clipId == clip.clipId());
    assert(desc->fileUri == "/nvme/clips/evt-123.mp4");
}

int main() {
    std::cout << "Running S4 Domain & Application Tests...\n";
    test_deve_calcular_duracao_corretamente_quando_janela_for_valida();
    test_deve_lancar_excecao_quando_start_time_for_maior_ou_igual_ao_end_time();
    test_deve_extrair_clipe_e_consultar_via_controller();
    std::cout << "All S4 tests passed successfully!\n";
    return 0;
}
