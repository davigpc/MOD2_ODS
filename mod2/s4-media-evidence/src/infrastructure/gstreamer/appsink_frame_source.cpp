#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"

#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

// Um access unit por buffer, em Annex-B: concatenar quadros produz um .h264
// valido e nenhum buffer traz "meio quadro".
constexpr const char* kH264Caps = "video/x-h264,stream-format=byte-stream,alignment=au";

// sync=false porque isto nao e um player (nao esperar o relogio de exibicao);
// drop=false porque descartar quadro em silencio produziria um clipe com
// buraco invisivel — preferimos backpressure, que aparece nas metricas.
constexpr const char* kAppsinkTail =
    "appsink name=ods_sink emit-signals=true sync=false max-buffers=8 drop=false";

} // namespace

std::string GStreamerAppsinkFrameSource::jetsonCsiH264Pipeline(const CsiCameraOptions& o) {
    return "nvarguscamerasrc sensor-id=" + std::to_string(o.sensorId) +
           " ! video/x-raw(memory:NVMM),width=" + std::to_string(o.width) +
           ",height=" + std::to_string(o.height) + ",framerate=" + std::to_string(o.fps) +
           "/1 ! nvv4l2h264enc bitrate=" + std::to_string(o.bitrateBps) +
           " iframeinterval=" + std::to_string(o.gop) +
           " idrinterval=" + std::to_string(o.gop) +
           // insert-sps-pps + config-interval=-1 repetem os parametros do codec
           // em TODO keyframe; sem isso, um trecho que comeca no meio do stream
           // nao tem como ser decodificado.
           " insert-sps-pps=true ! h264parse config-interval=-1 ! " + kH264Caps + " ! " +
           kAppsinkTail;
}

std::string GStreamerAppsinkFrameSource::shmH264Pipeline(const std::string& socketPath) {
    return "shmsrc socket-path=" + socketPath +
           " is-live=true do-timestamp=true ! h264parse config-interval=-1 ! " + kH264Caps +
           " ! " + kAppsinkTail;
}

std::string GStreamerAppsinkFrameSource::fileReplayH264Pipeline(const std::string& path) {
    return "filesrc location=" + path + " ! qtdemux ! h264parse config-interval=-1 ! " +
           kH264Caps + " ! " + kAppsinkTail;
}

std::string GStreamerAppsinkFrameSource::testPatternH264Pipeline(
    int width, int height, int fps, int bitrateKbps, int gop
) {
    return "videotestsrc is-live=true ! video/x-raw,width=" + std::to_string(width) +
           ",height=" + std::to_string(height) + ",framerate=" + std::to_string(fps) +
           "/1 ! x264enc tune=zerolatency bitrate=" + std::to_string(bitrateKbps) +
           " key-int-max=" + std::to_string(gop) + " ! h264parse config-interval=-1 ! " +
           kH264Caps + " ! " + kAppsinkTail;
}

#if defined(ODS_S4_WITH_GSTREAMER)

} // namespace ods::s4::infrastructure

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

namespace ods::s4::infrastructure {

struct GStreamerAppsinkFrameSource::Impl {
    GstElement* pipeline{nullptr};
    GstElement* sink{nullptr};
    GMainLoop* loop{nullptr};
    GThread* loopThread{nullptr};
    domain::FrameCallback onFrame;
    std::string sessionId;
};

namespace {

// capture_ts = base_time do pipeline + PTS do buffer (relogio monotonico).
// Ponto de acoplamento com B2: quando TRA-1/TRA-2 fecharem a base de tempo
// oficial (por exemplo o timestamp que P4 grava via GstReferenceTimestampMeta),
// basta trocar esta funcao.
domain::Nanoseconds capture_timestamp(GstElement* pipeline, GstBuffer* buffer) {
    return static_cast<domain::Nanoseconds>(gst_element_get_base_time(pipeline)) +
           static_cast<domain::Nanoseconds>(GST_BUFFER_PTS(buffer));
}

GstFlowReturn on_new_sample(GstAppSink* sink, gpointer userData) {
    auto* impl = static_cast<GStreamerAppsinkFrameSource::Impl*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo info;
    if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    domain::CapturedFrame frame;
    frame.captureTsNs = capture_timestamp(impl->pipeline, buffer);
    frame.data = static_cast<const std::uint8_t*>(info.data);
    frame.length = info.size;
    // Sem a flag DELTA_UNIT o buffer e um keyframe.
    frame.isKeyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    frame.sessionId = impl->sessionId;
    if (impl->onFrame) {
        impl->onFrame(frame);
    }

    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

} // namespace

GStreamerAppsinkFrameSource::GStreamerAppsinkFrameSource(
    std::string pipelineDescription,
    std::string sessionId
) : m_impl(new Impl()),
    m_pipelineDescription(std::move(pipelineDescription)),
    m_sessionId(std::move(sessionId)) {
    m_impl->sessionId = m_sessionId;
}

GStreamerAppsinkFrameSource::~GStreamerAppsinkFrameSource() {
    stop();
    delete m_impl;
}

void GStreamerAppsinkFrameSource::start(domain::FrameCallback onFrame) {
    if (isRunning()) {
        return;
    }
    gst_init(nullptr, nullptr);
    m_impl->onFrame = std::move(onFrame);

    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(m_pipelineDescription.c_str(), &error);
    if (m_impl->pipeline == nullptr) {
        const std::string message = error != nullptr ? error->message : "unknown";
        if (error != nullptr) {
            g_error_free(error);
        }
        throw domain::FrameSourceError("invalid pipeline: " + message);
    }

    m_impl->sink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "ods_sink");
    if (m_impl->sink == nullptr) {
        throw domain::FrameSourceError("pipeline must end in 'appsink name=ods_sink'");
    }
    g_signal_connect(m_impl->sink, "new-sample", G_CALLBACK(on_new_sample), m_impl);

    m_impl->loop = g_main_loop_new(nullptr, FALSE);
    m_impl->loopThread = g_thread_new(
        "ods-gst-loop",
        [](gpointer data) -> gpointer {
            g_main_loop_run(static_cast<GMainLoop*>(data));
            return nullptr;
        },
        m_impl->loop
    );

    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stop();
        throw domain::FrameSourceError("pipeline could not reach PLAYING");
    }
}

void GStreamerAppsinkFrameSource::stop() {
    if (m_impl->pipeline != nullptr) {
        gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
        gst_object_unref(m_impl->pipeline);
        m_impl->pipeline = nullptr;
        m_impl->sink = nullptr;
    }
    if (m_impl->loop != nullptr) {
        g_main_loop_quit(m_impl->loop);
        if (m_impl->loopThread != nullptr) {
            g_thread_join(m_impl->loopThread);
            m_impl->loopThread = nullptr;
        }
        g_main_loop_unref(m_impl->loop);
        m_impl->loop = nullptr;
    }
}

bool GStreamerAppsinkFrameSource::isRunning() const {
    return m_impl != nullptr && m_impl->pipeline != nullptr;
}

#else // sem GStreamer: os pipelines acima continuam disponiveis como texto

struct GStreamerAppsinkFrameSource::Impl {};

GStreamerAppsinkFrameSource::GStreamerAppsinkFrameSource(
    std::string pipelineDescription,
    std::string sessionId
) : m_impl(nullptr),
    m_pipelineDescription(std::move(pipelineDescription)),
    m_sessionId(std::move(sessionId)) {}

GStreamerAppsinkFrameSource::~GStreamerAppsinkFrameSource() = default;

void GStreamerAppsinkFrameSource::start(domain::FrameCallback) {
    throw domain::FrameSourceError(
        "built without GStreamer: rebuild with -DODS_S4_WITH_GSTREAMER=ON"
    );
}

void GStreamerAppsinkFrameSource::stop() {}

bool GStreamerAppsinkFrameSource::isRunning() const { return false; }

#endif

} // namespace ods::s4::infrastructure
