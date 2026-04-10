#include "dfn3_filter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include <util/bmem.h>
}

namespace {

constexpr const char *kSettingAttenLimDb = "atten_lim_db";
constexpr const char *kSettingPostFilterBeta = "post_filter_beta";
constexpr const char *kSettingAdaptiveQueue = "adaptive_queue";
constexpr const char *kSettingShowDebugDetails = "show_debug_details";
constexpr const char *kPropertyRuntimeStatusInfo = "runtime_status_info";
constexpr const char *kPropertyRuntimeGroup = "runtime_group";
constexpr const char *kPropertyProcessingGroup = "processing_group";

constexpr int64_t kNsPerSecond = 1000000000LL;
constexpr int kWorkerIdleWaitMs = 4;
constexpr uint64_t kAdaptiveQueueWindow = 150;
constexpr double kAdaptiveQueueIncreaseThreshold = 1.5;
constexpr double kAdaptiveQueueDecreaseThreshold = 0.75;

std::string VFormat(const char *fmt, va_list args)
{
    if (!fmt) {
        return {};
    }

    va_list args_copy;
    va_copy(args_copy, args);
    const int needed = vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    if (needed <= 0) {
        return {};
    }

    std::string out(static_cast<size_t>(needed), '\0');
    vsnprintf(out.data(), out.size() + 1, fmt, args);
    return out;
}

void LogSource(obs_source_t *source, int level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    std::string msg = VFormat(fmt, args);
    va_end(args);

    const char *name = source ? obs_source_get_name(source) : "unknown";
    blog(level, "[DFN3 Noise Suppress: '%s'] %s", name, msg.c_str());
}

uint64_t AbsDiffU64(uint64_t a, uint64_t b)
{
    return (a >= b) ? (a - b) : (b - a);
}

} // namespace

Dfn3NoiseSuppressFilter::Dfn3NoiseSuppressFilter(obs_source_t *context) : context_(context)
{
    StartWorker();
    obs_add_tick_callback(&Dfn3NoiseSuppressFilter::TickCallback, this);
}

Dfn3NoiseSuppressFilter::~Dfn3NoiseSuppressFilter()
{
    obs_remove_tick_callback(&Dfn3NoiseSuppressFilter::TickCallback, this);
    StopWorker();

    std::scoped_lock lock(input_mutex_, output_mutex_, config_mutex_);
    DestroyResamplersLocked();
    runtimes_.clear();
}

void Dfn3NoiseSuppressFilter::StartWorker()
{
    stop_worker_.store(false);
    worker_thread_ = std::thread(&Dfn3NoiseSuppressFilter::WorkerLoop, this);

#ifdef _WIN32
    if (worker_thread_.joinable()) {
        SetThreadPriority(worker_thread_.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);
    }
#endif
}

void Dfn3NoiseSuppressFilter::StopWorker()
{
    stop_worker_.store(true);
    input_cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void Dfn3NoiseSuppressFilter::RequestRebuild()
{
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        rebuild_requested_ = true;
    }

    controls_dirty_.store(true);
    runtime_ready_.store(false);
    input_cv_.notify_one();
}

void Dfn3NoiseSuppressFilter::SetLastError(const std::string &error)
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    last_error_ = error;
}

void Dfn3NoiseSuppressFilter::ClearLastError()
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    last_error_.clear();
}

std::string Dfn3NoiseSuppressFilter::GetLastError() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    return last_error_;
}

void Dfn3NoiseSuppressFilter::ApplyRuntimeControls()
{
    Settings settings_copy;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        settings_copy = settings_;
    }

    for (auto &runtime : runtimes_) {
        std::string err;
        if (!runtime.SetAttenLim(settings_copy.atten_lim_db, &err)) {
            if (!err.empty()) {
                LogSource(context_, LOG_WARNING, "Failed to apply attenuation limit: %s", err.c_str());
            }
        }

        err.clear();
        if (!runtime.SetPostFilterBeta(settings_copy.post_filter_beta, &err)) {
            if (!err.empty()) {
                LogSource(context_, LOG_WARNING, "Failed to apply post filter beta: %s", err.c_str());
            }
        }
    }

    if (!settings_copy.adaptive_queue) {
        queue_hops_.store(kMinQueueHops);
    }
}

void Dfn3NoiseSuppressFilter::WorkerLoop()
{
    while (!stop_worker_.load()) {
        bool rebuild_now = false;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            rebuild_now = rebuild_requested_;
            rebuild_requested_ = false;
        }

        if (rebuild_now) {
            if (!RebuildRuntimeLocked()) {
                runtime_ready_.store(false);
            }
        }

        if (runtime_ready_.load() && controls_dirty_.exchange(false)) {
            ApplyRuntimeControls();
        }

        bool processed = false;
        while (!stop_worker_.load() && runtime_ready_.load() && ProcessOneHop()) {
            processed = true;
        }

        if (!processed) {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait_for(lock, std::chrono::milliseconds(kWorkerIdleWaitMs));
        }
    }
}

bool Dfn3NoiseSuppressFilter::RebuildRuntimeLocked()
{
    uint32_t sample_rate = 0;
    size_t channels = 0;
    float atten_lim_db = 100.0f;

    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        sample_rate = sample_rate_;
        channels = channels_;
        atten_lim_db = settings_.atten_lim_db;
    }

    if (sample_rate == 0 || channels == 0 || channels > kMaxChannels) {
        return false;
    }

    const std::string model_path = ResolveModelPath();
    const std::vector<std::string> library_candidates = BuildLibraryCandidates();

    std::vector<DeepFilterRuntime> new_runtimes;
    new_runtimes.resize(channels);

    size_t frame_length = 0;
    for (size_t i = 0; i < channels; ++i) {
        std::string create_err;
        if (!new_runtimes[i].Create(library_candidates, model_path, atten_lim_db, "warn", &create_err)) {
            SetLastError(create_err);
            LogSource(context_, LOG_WARNING, "Runtime init failed: %s", create_err.c_str());
            return false;
        }

        if (frame_length == 0) {
            frame_length = new_runtimes[i].FrameLength();
        } else if (frame_length != new_runtimes[i].FrameLength()) {
            const std::string mismatch_err = "Inconsistent frame length across runtime instances.";
            SetLastError(mismatch_err);
            LogSource(context_, LOG_WARNING, "%s", mismatch_err.c_str());
            return false;
        }
    }

    if (frame_length == 0) {
        const std::string err = "DeepFilter frame length resolved to zero.";
        SetLastError(err);
        LogSource(context_, LOG_WARNING, "%s", err.c_str());
        return false;
    }

    {
        std::scoped_lock lock(input_mutex_, output_mutex_, packet_mutex_);
        runtimes_ = std::move(new_runtimes);
        if (!ReconfigureBuffersLocked(sample_rate, channels, frame_length)) {
            runtimes_.clear();
            FlushQueuesLocked();
            reset_count_.fetch_add(1);
            const std::string cfg_err = GetLastError();
            LogSource(context_, LOG_WARNING, "%s",
                      cfg_err.empty() ? "Runtime reconfiguration failed." : cfg_err.c_str());
            return false;
        }
        FlushQueuesLocked();
        reset_count_.fetch_add(1);
    }

    ClearLastError();
    controls_dirty_.store(true);
    runtime_ready_.store(true);
    LogSource(context_, LOG_INFO, "Runtime ready. sr=%u channels=%zu hop=%zu", sample_rate, channels, frame_length);
    return true;
}

bool Dfn3NoiseSuppressFilter::ReconfigureBuffersLocked(uint32_t sample_rate, size_t channels, size_t hop_size)
{
    sample_rate_ = sample_rate;
    channels_ = channels;
    hop_size_ = hop_size;
    queue_hops_.store(kMinQueueHops);

    const size_t input_capacity_samples = hop_size_ * kMaxInputQueueHops;
    const size_t output_capacity_samples = hop_size_ * (kMaxOutputQueueHops + 4);
    const size_t host_capacity_samples =
        static_cast<size_t>((std::max<uint32_t>(sample_rate_, kModelSampleRate) * (kMaxOutputQueueHops + 4U)) / 100U);
    const size_t output_storage_frames = std::max(kMaxExpectedCallbackFrames, host_capacity_samples);

    input_queues_48k_.assign(channels_, SampleRingBuffer(input_capacity_samples));
    output_queues_48k_.assign(channels_, SampleRingBuffer(output_capacity_samples));
    host_output_queues_.assign(channels_, SampleRingBuffer(host_capacity_samples));

    hop_input_scratch_.assign(channels_, std::vector<float>(hop_size_, 0.0f));
    hop_output_scratch_.assign(channels_, std::vector<float>(hop_size_, 0.0f));
    resample_hop_scratch_.assign(channels_, std::vector<float>(hop_size_, 0.0f));

    output_storage_.assign(channels_ * output_storage_frames, 0.0f);
    memset(&output_audio_, 0, sizeof(output_audio_));

    rolling_queue_depth_sum_.store(0.0);
    rolling_queue_depth_count_.store(0);
    resampler_from_model_ts_offset_ns_.store(0);

    DestroyResamplersLocked();
    if (!CreateResamplersLocked(sample_rate_, channels_)) {
        SetLastError("Failed to create required audio resamplers for stream format.");
        return false;
    }

    return true;
}

void Dfn3NoiseSuppressFilter::DestroyResamplersLocked()
{
    if (resampler_to_model_) {
        audio_resampler_destroy(resampler_to_model_);
        resampler_to_model_ = nullptr;
    }

    if (resampler_from_model_) {
        audio_resampler_destroy(resampler_from_model_);
        resampler_from_model_ = nullptr;
    }
}

bool Dfn3NoiseSuppressFilter::CreateResamplersLocked(uint32_t sample_rate, size_t channels)
{
    if (sample_rate == kModelSampleRate) {
        return true;
    }

    const enum speaker_layout layout = LayoutFromChannels(channels);
    if (layout == SPEAKERS_UNKNOWN) {
        return false;
    }

    struct resample_info src{};
    src.samples_per_sec = sample_rate;
    src.format = AUDIO_FORMAT_FLOAT_PLANAR;
    src.speakers = layout;

    struct resample_info model{};
    model.samples_per_sec = kModelSampleRate;
    model.format = AUDIO_FORMAT_FLOAT_PLANAR;
    model.speakers = layout;

    resampler_to_model_ = audio_resampler_create(&model, &src);
    if (!resampler_to_model_) {
        return false;
    }

    resampler_from_model_ = audio_resampler_create(&src, &model);
    if (!resampler_from_model_) {
        audio_resampler_destroy(resampler_to_model_);
        resampler_to_model_ = nullptr;
        return false;
    }

    return true;
}

void Dfn3NoiseSuppressFilter::FlushQueuesLocked()
{
    for (auto &q : input_queues_48k_) {
        q.Clear();
    }
    for (auto &q : output_queues_48k_) {
        q.Clear();
    }
    for (auto &q : host_output_queues_) {
        q.Clear();
    }
    packet_queue_head_ = 0;
    packet_queue_size_ = 0;
    resampler_from_model_ts_offset_ns_.store(0);
}

bool Dfn3NoiseSuppressFilter::PushPacketLocked(const PacketInfo &packet)
{
    if (packet_queue_size_ >= kMaxPacketQueueEntries) {
        return false;
    }

    const size_t tail = (packet_queue_head_ + packet_queue_size_) % kMaxPacketQueueEntries;
    packet_queue_[tail] = packet;
    ++packet_queue_size_;
    return true;
}

bool Dfn3NoiseSuppressFilter::PeekPacketLocked(PacketInfo *packet) const
{
    if (!packet || packet_queue_size_ == 0) {
        return false;
    }

    *packet = packet_queue_[packet_queue_head_];
    return true;
}

void Dfn3NoiseSuppressFilter::PopPacketLocked()
{
    if (packet_queue_size_ == 0) {
        return;
    }

    packet_queue_head_ = (packet_queue_head_ + 1) % kMaxPacketQueueEntries;
    --packet_queue_size_;
}

bool Dfn3NoiseSuppressFilter::EnsureConfiguredForStream(uint32_t sample_rate, size_t channels)
{
    if (channels == 0 || channels > kMaxChannels) {
        return false;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        if (sample_rate_ != sample_rate || channels_ != channels) {
            sample_rate_ = sample_rate;
            channels_ = channels;
            changed = true;
        }
    }

    if (changed) {
        RequestRebuild();
    }

    return true;
}

bool Dfn3NoiseSuppressFilter::PushInputLocked(const obs_audio_data *audio, uint64_t *input_ts_offset_ns)
{
    if (!audio) {
        return false;
    }

    if (input_ts_offset_ns) {
        *input_ts_offset_ns = 0;
    }

    const size_t frames = static_cast<size_t>(audio->frames);

    for (size_t c = 0; c < channels_; ++c) {
        if (input_queues_48k_[c].AvailableWrite() < frames) {
            return false;
        }
    }

    for (size_t c = 0; c < channels_; ++c) {
        auto *src = reinterpret_cast<const float *>(audio->data[c]);
        if (!src || !input_queues_48k_[c].PushBack(src, frames)) {
            return false;
        }
    }

    return true;
}

bool Dfn3NoiseSuppressFilter::PushInputFromResamplerLocked(const obs_audio_data *audio, uint64_t *input_ts_offset_ns)
{
    if (!audio || !resampler_to_model_) {
        return false;
    }

    uint8_t *resampled[kMaxChannels] = {nullptr, nullptr};
    uint32_t out_frames = 0;
    uint64_t ts_offset = 0;

    const bool ok = audio_resampler_resample(resampler_to_model_, resampled, &out_frames, &ts_offset,
                                             const_cast<const uint8_t *const *>(audio->data), audio->frames);
    if (!ok) {
        return false;
    }

    if (input_ts_offset_ns) {
        *input_ts_offset_ns = ts_offset;
    }

    if (out_frames == 0) {
        return true;
    }

    for (size_t c = 0; c < channels_; ++c) {
        if (input_queues_48k_[c].AvailableWrite() < out_frames) {
            return false;
        }
    }

    for (size_t c = 0; c < channels_; ++c) {
        const auto *src = reinterpret_cast<const float *>(resampled[c]);
        if (!src || !input_queues_48k_[c].PushBack(src, out_frames)) {
            return false;
        }
    }

    return true;
}

bool Dfn3NoiseSuppressFilter::ProcessOneHop()
{
    if (!runtime_ready_.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        for (size_t c = 0; c < channels_; ++c) {
            if (output_queues_48k_[c].AvailableWrite() < hop_size_) {
                return false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        for (size_t c = 0; c < channels_; ++c) {
            if (input_queues_48k_[c].Size() < hop_size_) {
                return false;
            }
        }

        for (size_t c = 0; c < channels_; ++c) {
            if (!input_queues_48k_[c].PopFront(hop_input_scratch_[c].data(), hop_size_)) {
                return false;
            }
        }
    }

    for (size_t c = 0; c < channels_; ++c) {
        float lsnr = 0.0f;
        std::string err;
        if (!runtimes_[c].ProcessFrame(hop_input_scratch_[c].data(), hop_output_scratch_[c].data(), &lsnr, &err)) {
            SetLastError(err);
            LogSource(context_, LOG_WARNING, "Processing failed: %s", err.c_str());
            RequestRebuild();
            return false;
        }

        const std::string runtime_msg = runtimes_[c].PollLogMessage();
        if (!runtime_msg.empty()) {
            LogSource(context_, LOG_DEBUG, "libDF: %s", runtime_msg.c_str());
        }
    }

    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        for (size_t c = 0; c < channels_; ++c) {
            if (!output_queues_48k_[c].PushBack(hop_output_scratch_[c].data(), hop_size_)) {
                return false;
            }
        }
    }

    bool adaptive_queue_enabled = false;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        adaptive_queue_enabled = settings_.adaptive_queue;
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        const double depth_hops = static_cast<double>(input_queues_48k_[0].Size()) / static_cast<double>(hop_size_);
        rolling_queue_depth_sum_.store(rolling_queue_depth_sum_.load() + depth_hops);
        const uint64_t count = rolling_queue_depth_count_.fetch_add(1) + 1;

        if (adaptive_queue_enabled && count >= kAdaptiveQueueWindow) {
            const double avg_depth = rolling_queue_depth_sum_.load() / static_cast<double>(count);
            size_t queue_hops = queue_hops_.load();
            if (avg_depth > kAdaptiveQueueIncreaseThreshold && queue_hops < kMaxInputQueueHops) {
                ++queue_hops;
                queue_hops_.store(queue_hops);
                LogSource(context_, LOG_INFO, "Increasing queue depth to %zu hops (avg depth %.2f).", queue_hops,
                          avg_depth);
            } else if (avg_depth < kAdaptiveQueueDecreaseThreshold && queue_hops > kMinQueueHops) {
                --queue_hops;
                queue_hops_.store(queue_hops);
                LogSource(context_, LOG_INFO, "Decreasing queue depth to %zu hops (avg depth %.2f).", queue_hops,
                          avg_depth);
            }
            rolling_queue_depth_sum_.store(0.0);
            rolling_queue_depth_count_.store(0);
        }
    }

    return true;
}

bool Dfn3NoiseSuppressFilter::PrepareHostOutputLocked(size_t needed_frames, size_t *queue_dwell_frames)
{
    if (queue_dwell_frames) {
        *queue_dwell_frames = 0;
    }

    const size_t queue_hold_model_samples = queue_hops_.load() * hop_size_;

    if (sample_rate_ == kModelSampleRate) {
        std::lock_guard<std::mutex> output_lock(output_mutex_);
        const size_t capacity = output_queues_48k_.empty() ? 0 : output_queues_48k_[0].Capacity();
        const size_t max_hold_samples = (capacity > needed_frames) ? (capacity - needed_frames) : 0;
        const size_t queue_hold_samples = std::min(queue_hold_model_samples, max_hold_samples);
        const size_t required_samples = needed_frames + queue_hold_samples;

        for (size_t c = 0; c < channels_; ++c) {
            if (output_queues_48k_[c].Size() < required_samples) {
                return false;
            }
        }

        if (queue_dwell_frames) {
            const size_t depth = output_queues_48k_[0].Size();
            *queue_dwell_frames = (depth > needed_frames) ? (depth - needed_frames) : 0;
        }
        return true;
    }

    const size_t queue_hold_host_samples = static_cast<size_t>(std::ceil(
        static_cast<double>(queue_hold_model_samples) * static_cast<double>(sample_rate_) /
        static_cast<double>(kModelSampleRate)));
    const size_t host_capacity = host_output_queues_.empty() ? 0 : host_output_queues_[0].Capacity();
    const size_t max_hold_host_samples = (host_capacity > needed_frames) ? (host_capacity - needed_frames) : 0;
    const size_t required_host_samples = needed_frames + std::min(queue_hold_host_samples, max_hold_host_samples);

    const size_t max_resampled_frames =
        static_cast<size_t>(std::ceil(static_cast<double>(hop_size_) * static_cast<double>(sample_rate_) /
                                      static_cast<double>(kModelSampleRate))) +
        16;

    while (true) {
        {
            std::lock_guard<std::mutex> output_lock(output_mutex_);
            if (host_output_queues_[0].Size() >= required_host_samples) {
                break;
            }
        }

        {
            std::lock_guard<std::mutex> output_lock(output_mutex_);
            for (size_t c = 0; c < channels_; ++c) {
                if (output_queues_48k_[c].Size() < hop_size_) {
                    return false;
                }
                if (host_output_queues_[c].AvailableWrite() < max_resampled_frames) {
                    return false;
                }
            }

            for (size_t c = 0; c < channels_; ++c) {
                if (!output_queues_48k_[c].PopFront(resample_hop_scratch_[c].data(), hop_size_)) {
                    return false;
                }
            }
        }

        std::array<const uint8_t *, kMaxChannels> in_ptrs{};
        for (size_t c = 0; c < channels_; ++c) {
            in_ptrs[c] = reinterpret_cast<const uint8_t *>(resample_hop_scratch_[c].data());
        }

        uint8_t *resampled[kMaxChannels] = {nullptr, nullptr};
        uint32_t out_frames = 0;
        uint64_t ts_offset = 0;

        const bool ok = audio_resampler_resample(resampler_from_model_, resampled, &out_frames, &ts_offset,
                                                 in_ptrs.data(), static_cast<uint32_t>(hop_size_));
        if (!ok) {
            return false;
        }
        resampler_from_model_ts_offset_ns_.store(ts_offset);

        if (out_frames == 0) {
            continue;
        }

        {
            std::lock_guard<std::mutex> output_lock(output_mutex_);
            for (size_t c = 0; c < channels_; ++c) {
                if (host_output_queues_[c].AvailableWrite() < out_frames) {
                    return false;
                }
            }

            for (size_t c = 0; c < channels_; ++c) {
                const auto *src = reinterpret_cast<const float *>(resampled[c]);
                if (!src || !host_output_queues_[c].PushBack(src, out_frames)) {
                    return false;
                }
            }
        }
    }

    if (queue_dwell_frames) {
        std::lock_guard<std::mutex> output_lock(output_mutex_);
        const size_t depth = host_output_queues_[0].Size();
        *queue_dwell_frames = (depth > needed_frames) ? (depth - needed_frames) : 0;
    }

    return true;
}

bool Dfn3NoiseSuppressFilter::PopPacketToOutput(const PacketInfo &packet)
{
    if (packet.frames == 0 || channels_ == 0) {
        return false;
    }

    const size_t frames = static_cast<size_t>(packet.frames);
    const size_t total = frames * channels_;
    if (output_storage_.size() < total) {
        return false;
    }

    size_t queue_dwell_frames = 0;

    if (sample_rate_ == kModelSampleRate) {
        std::lock_guard<std::mutex> output_lock(output_mutex_);

        const size_t queue_depth_frames = output_queues_48k_.empty() ? 0 : output_queues_48k_[0].Size();
        if (queue_depth_frames < frames) {
            return false;
        }
        queue_dwell_frames = queue_depth_frames - frames;

        for (size_t c = 0; c < channels_; ++c) {
            float *dst = output_storage_.data() + (c * frames);
            if (!output_queues_48k_[c].PopFront(dst, frames)) {
                return false;
            }
            output_audio_.data[c] = reinterpret_cast<uint8_t *>(dst);
        }
    } else {
        std::lock_guard<std::mutex> output_lock(output_mutex_);

        const size_t queue_depth_frames = host_output_queues_.empty() ? 0 : host_output_queues_[0].Size();
        if (queue_depth_frames < frames) {
            return false;
        }
        queue_dwell_frames = queue_depth_frames - frames;

        for (size_t c = 0; c < channels_; ++c) {
            float *dst = output_storage_.data() + (c * frames);
            if (!host_output_queues_[c].PopFront(dst, frames)) {
                return false;
            }
            output_audio_.data[c] = reinterpret_cast<uint8_t *>(dst);
        }
    }

    output_audio_.frames = packet.frames;

    const uint64_t output_ts_offset_ns =
        (sample_rate_ == kModelSampleRate) ? 0 : resampler_from_model_ts_offset_ns_.load();
    const uint64_t latency_ns =
        ComputeLatencyNs(queue_dwell_frames, packet.input_ts_offset_ns, output_ts_offset_ns);
    output_audio_.timestamp = (packet.timestamp_ns > latency_ns) ? (packet.timestamp_ns - latency_ns) : 0;
    return true;
}

uint64_t Dfn3NoiseSuppressFilter::ModelDelaySamples() const
{
    // Standard DFN3 lookahead profile.
    return static_cast<uint64_t>(hop_size_ * 3U);
}

uint64_t Dfn3NoiseSuppressFilter::ComputeLatencyNs(size_t queue_dwell_frames,
                                                   uint64_t input_ts_offset_ns,
                                                   uint64_t output_ts_offset_ns) const
{
    if (hop_size_ == 0 || sample_rate_ == 0) {
        return 0;
    }

    const uint64_t model_delay_samples = ModelDelaySamples();
    const uint64_t model_delay_ns = static_cast<uint64_t>((static_cast<long double>(model_delay_samples) * kNsPerSecond) /
                                                          static_cast<long double>(kModelSampleRate));
    const uint64_t queue_delay_ns =
        static_cast<uint64_t>((static_cast<long double>(queue_dwell_frames) * kNsPerSecond) /
                              static_cast<long double>(sample_rate_));

    return model_delay_ns + queue_delay_ns + input_ts_offset_ns + output_ts_offset_ns;
}

std::string Dfn3NoiseSuppressFilter::ResolveModelPath() const
{
    const char *default_name = "models/DeepFilterNet3_onnx.tar.gz";

    char *path = obs_module_file(default_name);
    if (!path) {
        return {};
    }

    std::string resolved(path);
    bfree(path);
    return resolved;
}

std::vector<std::string> Dfn3NoiseSuppressFilter::BuildLibraryCandidates() const
{
    std::vector<std::string> candidates;

#ifdef _WIN32
    if (char *path = obs_module_file("deepfilter.dll")) {
        candidates.emplace_back(path);
        bfree(path);
    }
    if (char *path = obs_module_file("bin/deepfilter.dll")) {
        candidates.emplace_back(path);
        bfree(path);
    }
#else
    if (char *path = obs_module_file("libdeepfilter.so")) {
        candidates.emplace_back(path);
        bfree(path);
    }
    if (char *path = obs_module_file("libdeepfilter.dylib")) {
        candidates.emplace_back(path);
        bfree(path);
    }
#endif

    return candidates;
}

enum speaker_layout Dfn3NoiseSuppressFilter::LayoutFromChannels(size_t channels)
{
    switch (channels) {
    case 1:
        return SPEAKERS_MONO;
    case 2:
        return SPEAKERS_STEREO;
    default:
        return SPEAKERS_UNKNOWN;
    }
}

void Dfn3NoiseSuppressFilter::Update(obs_data_t *settings)
{
    if (!settings) {
        return;
    }

    Settings next;
    next.atten_lim_db = static_cast<float>(obs_data_get_double(settings, kSettingAttenLimDb));
    next.post_filter_beta = static_cast<float>(obs_data_get_double(settings, kSettingPostFilterBeta));
    next.adaptive_queue = obs_data_get_bool(settings, kSettingAdaptiveQueue);
    next.show_debug_details = obs_data_get_bool(settings, kSettingShowDebugDetails);

    bool requires_rebuild = false;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        requires_rebuild = (settings_.atten_lim_db != next.atten_lim_db) ||
                           (settings_.post_filter_beta != next.post_filter_beta) ||
                           (settings_.adaptive_queue != next.adaptive_queue);
        settings_ = std::move(next);
    }

    controls_dirty_.store(true);
    last_published_status_valid_ = false;

    if (requires_rebuild && !runtime_ready_.load()) {
        RequestRebuild();
    }
}

void Dfn3NoiseSuppressFilter::Tick(float seconds)
{
    if (!context_) {
        return;
    }

    status_refresh_accum_s_ += static_cast<double>(seconds);
    if (status_refresh_accum_s_ < 0.5) {
        return;
    }
    status_refresh_accum_s_ = 0.0;

    const std::string current_status = BuildStatusText(ShowDebugDetailsEnabled());
    if (!last_published_status_valid_ || current_status != last_published_status_text_) {
        last_published_status_text_ = current_status;
        last_published_status_valid_ = true;
        obs_source_update_properties(context_);
    }
}

void Dfn3NoiseSuppressFilter::TickCallback(void *param, float seconds)
{
    auto *filter = static_cast<Dfn3NoiseSuppressFilter *>(param);
    if (filter) {
        filter->Tick(seconds);
    }
}

bool Dfn3NoiseSuppressFilter::DebugDetailsModified(obs_properties_t *, obs_property_t *, obs_data_t *)
{
    return true;
}

void Dfn3NoiseSuppressFilter::Activate()
{
    last_timestamp_ns_ = 0;
    RequestRebuild();
}

void Dfn3NoiseSuppressFilter::Deactivate()
{
    last_timestamp_ns_ = 0;

    {
        std::scoped_lock lock(input_mutex_, output_mutex_, packet_mutex_);
        FlushQueuesLocked();
    }

    RequestRebuild();
}

bool Dfn3NoiseSuppressFilter::ShowDebugDetailsEnabled() const
{
    std::lock_guard<std::mutex> lock(config_mutex_);
    return settings_.show_debug_details;
}

std::string Dfn3NoiseSuppressFilter::BuildStatusText(bool show_debug_details) const
{
    std::ostringstream status;
    const std::string last_error = GetLastError();
    const bool ready = runtime_ready_.load();

    status << "Engine: " << (ready ? "Ready" : "Starting...") << "\n";
    status << "Model: Standard DeepFilterNet3 (packaged)";

    if (!last_error.empty()) {
        status << "\nIssue: " << last_error;
    } else if (!ready) {
        status << "\nState: waiting for active audio stream.";
    }

    if (show_debug_details) {
        status << "\n\nTechnical details";
        status << "\nQueue hops target: " << queue_hops_.load();
        status << "\nInput overflows: " << overflow_count_.load();
        status << "\nOutput underruns: " << underrun_count_.load();
        status << "\nResets: " << reset_count_.load();
    }

    return status.str();
}

obs_audio_data *Dfn3NoiseSuppressFilter::FilterAudio(obs_audio_data *audio)
{
    if (!audio) {
        return nullptr;
    }

    audio_t *ao = obs_get_audio();
    if (!ao) {
        return audio;
    }

    const struct audio_output_info *ao_info = audio_output_get_info(ao);
    if (!ao_info || ao_info->format != AUDIO_FORMAT_FLOAT_PLANAR) {
        return audio;
    }

    const uint32_t sample_rate = ao_info->samples_per_sec;
    size_t channels = audio_output_get_channels(ao);

    if (obs_source_t *parent = obs_filter_get_parent(context_)) {
        const enum speaker_layout layout = obs_source_get_speaker_layout(parent);
        const size_t parent_channels = static_cast<size_t>(get_audio_channels(layout));
        if (parent_channels > 0) {
            channels = std::min(channels, parent_channels);
        }
    }

    if (!EnsureConfiguredForStream(sample_rate, channels)) {
        return audio;
    }

    if (last_timestamp_ns_ != 0 && AbsDiffU64(last_timestamp_ns_, audio->timestamp) > kTimestampResetThresholdNs) {
        RequestRebuild();
    }
    last_timestamp_ns_ = audio->timestamp;

    if (!runtime_ready_.load()) {
        return audio;
    }

    bool packet_enqueued = false;
    {
        std::lock_guard<std::mutex> lock(packet_mutex_);
        packet_enqueued = PushPacketLocked({audio->frames, audio->timestamp, 0});
    }

    if (!packet_enqueued) {
        overflow_count_.fetch_add(1);
        {
            std::scoped_lock queue_lock(input_mutex_, output_mutex_, packet_mutex_);
            FlushQueuesLocked();
        }
        RequestRebuild();
        return audio;
    }

    bool queued_input = false;
    uint64_t input_ts_offset_ns = 0;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        if (sample_rate_ == kModelSampleRate) {
            queued_input = PushInputLocked(audio, &input_ts_offset_ns);
        } else {
            queued_input = PushInputFromResamplerLocked(audio, &input_ts_offset_ns);
        }
    }

    if (!queued_input) {
        overflow_count_.fetch_add(1);
        {
            std::scoped_lock lock(input_mutex_, output_mutex_, packet_mutex_);
            FlushQueuesLocked();
        }
        RequestRebuild();
        return audio;
    }

    if (input_ts_offset_ns != 0) {
        std::lock_guard<std::mutex> lock(packet_mutex_);
        if (packet_queue_size_ > 0) {
            const size_t tail = (packet_queue_head_ + packet_queue_size_ - 1) % kMaxPacketQueueEntries;
            packet_queue_[tail].input_ts_offset_ns = input_ts_offset_ns;
        }
    }

    input_cv_.notify_one();

    PacketInfo packet;
    {
        std::lock_guard<std::mutex> lock(packet_mutex_);
        if (!PeekPacketLocked(&packet)) {
            return nullptr;
        }
    }

    if (!PrepareHostOutputLocked(packet.frames, nullptr)) {
        underrun_count_.fetch_add(1);
        return nullptr;
    }

    if (!PopPacketToOutput(packet)) {
        underrun_count_.fetch_add(1);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(packet_mutex_);
        PopPacketLocked();
    }

    return &output_audio_;
}

const char *Dfn3NoiseSuppressFilter::GetName(void *)
{
    return "DeepFilterNet3 Noise Suppress";
}

void *Dfn3NoiseSuppressFilter::Create(obs_data_t *settings, obs_source_t *context)
{
    auto *filter = new Dfn3NoiseSuppressFilter(context);
    filter->Update(settings);
    return filter;
}

void Dfn3NoiseSuppressFilter::Destroy(void *data)
{
    delete static_cast<Dfn3NoiseSuppressFilter *>(data);
}

void Dfn3NoiseSuppressFilter::UpdateCallback(void *data, obs_data_t *settings)
{
    auto *filter = static_cast<Dfn3NoiseSuppressFilter *>(data);
    if (filter) {
        filter->Update(settings);
    }
}

obs_audio_data *Dfn3NoiseSuppressFilter::FilterAudioCallback(void *data, obs_audio_data *audio)
{
    auto *filter = static_cast<Dfn3NoiseSuppressFilter *>(data);
    if (!filter) {
        return audio;
    }
    return filter->FilterAudio(audio);
}

void Dfn3NoiseSuppressFilter::ActivateCallback(void *data)
{
    auto *filter = static_cast<Dfn3NoiseSuppressFilter *>(data);
    if (filter) {
        filter->Activate();
    }
}

void Dfn3NoiseSuppressFilter::DeactivateCallback(void *data)
{
    auto *filter = static_cast<Dfn3NoiseSuppressFilter *>(data);
    if (filter) {
        filter->Deactivate();
    }
}

void Dfn3NoiseSuppressFilter::GetDefaults(obs_data_t *settings)
{
    obs_data_set_default_double(settings, kSettingAttenLimDb, 100.0);
    obs_data_set_default_double(settings, kSettingPostFilterBeta, 0.0);
    obs_data_set_default_bool(settings, kSettingAdaptiveQueue, true);
    obs_data_set_default_bool(settings, kSettingShowDebugDetails, false);
}

obs_properties_t *Dfn3NoiseSuppressFilter::GetProperties(void *data)
{
    obs_properties_t *props = obs_properties_create();

    auto *filter = static_cast<Dfn3NoiseSuppressFilter *>(data);
    const bool show_debug_details = filter ? filter->ShowDebugDetailsEnabled() : false;
    const std::string status_text = filter ? filter->BuildStatusText(show_debug_details) : "Engine: starting...";
    const bool runtime_ready = filter ? filter->runtime_ready_.load() : false;

    obs_properties_t *status_group = obs_properties_create();
    obs_property_t *status_prop =
        obs_properties_add_text(status_group, kPropertyRuntimeStatusInfo, status_text.c_str(), OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(status_prop, true);
    obs_property_text_set_info_type(status_prop, runtime_ready ? OBS_TEXT_INFO_NORMAL : OBS_TEXT_INFO_WARNING);

    obs_property_t *debug_prop =
        obs_properties_add_bool(status_group, kSettingShowDebugDetails, "Show technical details (debug)");
    obs_property_set_modified_callback(debug_prop, Dfn3NoiseSuppressFilter::DebugDetailsModified);

    obs_properties_add_group(props, kPropertyRuntimeGroup, "Runtime Status", OBS_GROUP_NORMAL, status_group);

    obs_properties_t *processing_group = obs_properties_create();

    obs_property_t *atten =
        obs_properties_add_float_slider(processing_group, kSettingAttenLimDb, "Attenuation Limit (dB)", 0.0, 100.0, 1.0);
    obs_property_float_set_suffix(atten, " dB");

    obs_properties_add_float_slider(processing_group, kSettingPostFilterBeta, "Post Filter Beta", 0.0, 0.05, 0.001);

    obs_properties_add_bool(processing_group, kSettingAdaptiveQueue, "Enable Adaptive Queue");

    obs_properties_add_group(props, kPropertyProcessingGroup, "Processing", OBS_GROUP_NORMAL, processing_group);

    return props;
}

struct obs_source_info dfn3_noise_suppress_filter_info = [] {
    obs_source_info info{};
    info.id = Dfn3NoiseSuppressFilter::kFilterId;
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = Dfn3NoiseSuppressFilter::GetName;
    info.create = Dfn3NoiseSuppressFilter::Create;
    info.destroy = Dfn3NoiseSuppressFilter::Destroy;
    info.update = Dfn3NoiseSuppressFilter::UpdateCallback;
    info.filter_audio = Dfn3NoiseSuppressFilter::FilterAudioCallback;
    info.activate = Dfn3NoiseSuppressFilter::ActivateCallback;
    info.deactivate = Dfn3NoiseSuppressFilter::DeactivateCallback;
    info.get_defaults = Dfn3NoiseSuppressFilter::GetDefaults;
    info.get_properties = Dfn3NoiseSuppressFilter::GetProperties;
    return info;
}();
