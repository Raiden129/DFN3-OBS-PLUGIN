#include "dfn3_filter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

extern "C" {
#include <util/bmem.h>
}

namespace {

constexpr const char *kSettingAttenLimDb = "atten_lim_db";
constexpr const char *kSettingPostFilterBeta = "post_filter_beta";
constexpr const char *kSettingAdaptiveQueue = "adaptive_queue";
constexpr const char *kSettingCustomModelPath = "custom_model_path";
constexpr const char *kSettingCustomLibraryPath = "custom_library_path";

constexpr int64_t kNsPerSecond = 1000000000LL;

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

int64_t AbsDiffInt64(uint64_t a, uint64_t b)
{
    const int64_t ai = static_cast<int64_t>(a);
    const int64_t bi = static_cast<int64_t>(b);
    return llabs(ai - bi);
}

} // namespace

Dfn3NoiseSuppressFilter::Dfn3NoiseSuppressFilter(obs_source_t *context) : context_(context)
{
    StartWorker();
}

Dfn3NoiseSuppressFilter::~Dfn3NoiseSuppressFilter()
{
    StopWorker();

    std::scoped_lock lock(input_mutex_, output_mutex_, config_mutex_);
    DestroyResamplersLocked();
    runtimes_.clear();
}

void Dfn3NoiseSuppressFilter::StartWorker()
{
    stop_worker_.store(false);
    worker_thread_ = std::thread(&Dfn3NoiseSuppressFilter::WorkerLoop, this);
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

    runtime_ready_.store(false);
    input_cv_.notify_one();
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

        ApplyRuntimeControls();

        bool processed = false;
        while (!stop_worker_.load() && runtime_ready_.load() && ProcessOneHop()) {
            processed = true;
        }

        if (!processed) {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait_for(lock, std::chrono::milliseconds(4));
        }
    }
}

bool Dfn3NoiseSuppressFilter::RebuildRuntimeLocked()
{
    Settings settings_copy;
    uint32_t sample_rate = 0;
    size_t channels = 0;

    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        settings_copy = settings_;
        sample_rate = sample_rate_;
        channels = channels_;
    }

    if (sample_rate == 0 || channels == 0 || channels > kMaxChannels) {
        return false;
    }

    const std::string model_path = ResolveModelPath(settings_copy);
    const std::vector<std::string> library_candidates = BuildLibraryCandidates(settings_copy);

    std::vector<DeepFilterRuntime> new_runtimes;
    new_runtimes.resize(channels);

    size_t frame_length = 0;
    for (size_t i = 0; i < channels; ++i) {
        std::string err;
        if (!new_runtimes[i].Create(library_candidates, model_path, settings_copy.atten_lim_db, "warn", &err)) {
            last_error_ = err;
            LogSource(context_, LOG_WARNING, "Runtime init failed: %s", last_error_.c_str());
            return false;
        }

        if (frame_length == 0) {
            frame_length = new_runtimes[i].FrameLength();
        } else if (frame_length != new_runtimes[i].FrameLength()) {
            last_error_ = "Inconsistent frame length across runtime instances.";
            LogSource(context_, LOG_WARNING, "%s", last_error_.c_str());
            return false;
        }
    }

    if (frame_length == 0) {
        last_error_ = "DeepFilter frame length resolved to zero.";
        LogSource(context_, LOG_WARNING, "%s", last_error_.c_str());
        return false;
    }

    {
        std::scoped_lock lock(input_mutex_, output_mutex_);
        runtimes_ = std::move(new_runtimes);
        ReconfigureBuffersLocked(sample_rate, channels, frame_length);
        FlushQueuesLocked();
        reset_count_.fetch_add(1);
    }

    runtime_ready_.store(true);
    LogSource(context_, LOG_INFO, "Runtime ready. sr=%u channels=%zu hop=%zu", sample_rate, channels, frame_length);
    return true;
}

void Dfn3NoiseSuppressFilter::ReconfigureBuffersLocked(uint32_t sample_rate, size_t channels, size_t hop_size)
{
    sample_rate_ = sample_rate;
    channels_ = channels;
    hop_size_ = hop_size;
    queue_hops_.store(kMinQueueHops);

    const size_t input_capacity_samples = hop_size_ * kMaxInputQueueHops;
    const size_t output_capacity_samples = hop_size_ * (kMaxOutputQueueHops + 2);
    const size_t host_capacity_samples =
        static_cast<size_t>((std::max<uint32_t>(sample_rate_, kModelSampleRate) * (kMaxOutputQueueHops + 4U)) / 100U);

    input_queues_48k_.assign(channels_, SampleRingBuffer(input_capacity_samples));
    output_queues_48k_.assign(channels_, SampleRingBuffer(output_capacity_samples));
    host_output_queues_.assign(channels_, SampleRingBuffer(host_capacity_samples));

    hop_input_scratch_.assign(channels_, std::vector<float>(hop_size_, 0.0f));
    hop_output_scratch_.assign(channels_, std::vector<float>(hop_size_, 0.0f));
    resample_hop_scratch_.assign(channels_, std::vector<float>(hop_size_, 0.0f));

    output_storage_.assign(channels_ * 4096, 0.0f);
    memset(&output_audio_, 0, sizeof(output_audio_));

    rolling_queue_depth_sum_.store(0.0);
    rolling_queue_depth_count_.store(0);

    DestroyResamplersLocked();
    CreateResamplersLocked(sample_rate_, channels_);
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
    packet_queue_.clear();
}

bool Dfn3NoiseSuppressFilter::EnsureConfiguredForStream(uint32_t sample_rate, size_t channels)
{
    if (channels == 0 || channels > kMaxChannels) {
        if (channels > kMaxChannels) {
            LogSource(context_, LOG_WARNING, "Only up to %zu channels are supported in v1. Passing through.", kMaxChannels);
        }
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

bool Dfn3NoiseSuppressFilter::PushInputLocked(const obs_audio_data *audio)
{
    if (!audio) {
        return false;
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

bool Dfn3NoiseSuppressFilter::PushInputFromResamplerLocked(const obs_audio_data *audio)
{
    if (!audio || !resampler_to_model_) {
        return false;
    }

    uint8_t *resampled[kMaxChannels] = {nullptr, nullptr};
    uint32_t out_frames = 0;
    uint64_t ts_offset = 0;

    const bool ok = audio_resampler_resample(resampler_to_model_, resampled, &out_frames, &ts_offset,
                                             const_cast<const uint8_t *const *>(audio->data), audio->frames);
    UNUSED_PARAMETER(ts_offset);
    if (!ok) {
        return false;
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
            last_error_ = err;
            LogSource(context_, LOG_WARNING, "Processing failed: %s", last_error_.c_str());
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
            if (output_queues_48k_[c].AvailableWrite() < hop_size_) {
                return false;
            }
        }

        for (size_t c = 0; c < channels_; ++c) {
            if (!output_queues_48k_[c].PushBack(hop_output_scratch_[c].data(), hop_size_)) {
                return false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        const double depth_hops = static_cast<double>(input_queues_48k_[0].Size()) / static_cast<double>(hop_size_);
        rolling_queue_depth_sum_.store(rolling_queue_depth_sum_.load() + depth_hops);
        const uint64_t count = rolling_queue_depth_count_.fetch_add(1) + 1;

        Settings settings_copy;
        {
            std::lock_guard<std::mutex> cfg_lock(config_mutex_);
            settings_copy = settings_;
        }

        if (settings_copy.adaptive_queue && count >= 150) {
            const double avg_depth = rolling_queue_depth_sum_.load() / static_cast<double>(count);
            size_t queue_hops = queue_hops_.load();
            if (avg_depth > 1.5 && queue_hops < kMaxInputQueueHops) {
                ++queue_hops;
                queue_hops_.store(queue_hops);
                LogSource(context_, LOG_INFO, "Increasing queue depth to %zu hops (avg depth %.2f).", queue_hops,
                          avg_depth);
            } else if (avg_depth < 0.75 && queue_hops > kMinQueueHops) {
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

bool Dfn3NoiseSuppressFilter::PrepareHostOutputLocked(size_t needed_frames)
{
    const size_t queue_hold_model_samples = queue_hops_.load() * hop_size_;

    if (sample_rate_ == kModelSampleRate) {
        for (size_t c = 0; c < channels_; ++c) {
            if (output_queues_48k_[c].Size() < needed_frames + queue_hold_model_samples) {
                return false;
            }
        }
        return true;
    }

    const size_t queue_hold_host_samples =
        static_cast<size_t>(static_cast<double>(queue_hold_model_samples) * static_cast<double>(sample_rate_) /
                            static_cast<double>(kModelSampleRate));

    while (host_output_queues_[0].Size() < needed_frames + queue_hold_host_samples) {
        for (size_t c = 0; c < channels_; ++c) {
            if (output_queues_48k_[c].Size() < hop_size_) {
                return false;
            }
        }

        for (size_t c = 0; c < channels_; ++c) {
            if (!output_queues_48k_[c].PopFront(resample_hop_scratch_[c].data(), hop_size_)) {
                return false;
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
        UNUSED_PARAMETER(ts_offset);
        if (!ok) {
            return false;
        }

        if (out_frames == 0) {
            continue;
        }

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
        output_storage_.assign(total, 0.0f);
    }

    auto &source_queues = (sample_rate_ == kModelSampleRate) ? output_queues_48k_ : host_output_queues_;

    for (size_t c = 0; c < channels_; ++c) {
        float *dst = output_storage_.data() + (c * frames);
        if (!source_queues[c].PopFront(dst, frames)) {
            return false;
        }
        output_audio_.data[c] = reinterpret_cast<uint8_t *>(dst);
    }

    output_audio_.frames = packet.frames;

    const uint64_t latency_ns = ComputeLatencyNs();
    output_audio_.timestamp = (packet.timestamp_ns > latency_ns) ? (packet.timestamp_ns - latency_ns) : 0;
    return true;
}

uint64_t Dfn3NoiseSuppressFilter::ModelDelaySamples() const
{
    // Standard DFN3 lookahead profile.
    return static_cast<uint64_t>(hop_size_ * 3U);
}

uint64_t Dfn3NoiseSuppressFilter::ComputeLatencyNs() const
{
    if (hop_size_ == 0) {
        return 0;
    }

    const uint64_t model_delay = ModelDelaySamples();
    const uint64_t queue_delay = static_cast<uint64_t>(queue_hops_.load() * hop_size_);
    const uint64_t total_samples_model_sr = model_delay + queue_delay;

    return static_cast<uint64_t>((static_cast<long double>(total_samples_model_sr) * kNsPerSecond) /
                                 static_cast<long double>(kModelSampleRate));
}

std::string Dfn3NoiseSuppressFilter::ResolveModelPath(const Settings &settings) const
{
    if (!settings.custom_model_path.empty()) {
        return settings.custom_model_path;
    }

    const char *default_name = "models/DeepFilterNet3_onnx.tar.gz";

    char *path = obs_module_file(default_name);
    if (!path) {
        return {};
    }

    std::string resolved(path);
    bfree(path);
    return resolved;
}

std::vector<std::string> Dfn3NoiseSuppressFilter::BuildLibraryCandidates(const Settings &settings) const
{
    std::vector<std::string> candidates;

    if (!settings.custom_library_path.empty()) {
        candidates.push_back(settings.custom_library_path);
    }

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

    const char *model_path = obs_data_get_string(settings, kSettingCustomModelPath);
    if (model_path) {
        next.custom_model_path = model_path;
    }

    const char *library_path = obs_data_get_string(settings, kSettingCustomLibraryPath);
    if (library_path) {
        next.custom_library_path = library_path;
    }

    bool requires_rebuild = false;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        requires_rebuild = (settings_.custom_model_path != next.custom_model_path) ||
                           (settings_.custom_library_path != next.custom_library_path);
        settings_ = std::move(next);
    }

    if (requires_rebuild) {
        RequestRebuild();
    }
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

    const uint32_t sample_rate = audio_output_get_sample_rate(ao);
    const size_t channels = audio_output_get_channels(ao);

    if (!EnsureConfiguredForStream(sample_rate, channels)) {
        return audio;
    }

    if (last_timestamp_ns_ != 0 && AbsDiffInt64(last_timestamp_ns_, audio->timestamp) >
                                       static_cast<int64_t>(kTimestampResetThresholdNs)) {
        LogSource(context_, LOG_INFO, "Detected timestamp discontinuity. Triggering reset.");
        RequestRebuild();
    }
    last_timestamp_ns_ = audio->timestamp;

    if (!runtime_ready_.load()) {
        return audio;
    }

    bool queued_input = false;
    {
        std::scoped_lock lock(input_mutex_, output_mutex_);

        packet_queue_.push_back({audio->frames, audio->timestamp});

        if (sample_rate_ == kModelSampleRate) {
            queued_input = PushInputLocked(audio);
        } else {
            queued_input = PushInputFromResamplerLocked(audio);
        }

        if (!queued_input) {
            overflow_count_.fetch_add(1);
            FlushQueuesLocked();
            runtime_ready_.store(false);
            {
                std::lock_guard<std::mutex> cfg_lock(config_mutex_);
                rebuild_requested_ = true;
            }
            LogSource(context_, LOG_WARNING, "Input queue overflow. Bypassing and resetting runtime.");
            input_cv_.notify_one();
            return audio;
        }
    }

    input_cv_.notify_one();

    PacketInfo packet;
    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        if (packet_queue_.empty()) {
            return nullptr;
        }

        packet = packet_queue_.front();

        if (!PrepareHostOutputLocked(packet.frames)) {
            underrun_count_.fetch_add(1);
            return nullptr;
        }

        if (!PopPacketToOutput(packet)) {
            underrun_count_.fetch_add(1);
            return nullptr;
        }

        packet_queue_.pop_front();
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

void Dfn3NoiseSuppressFilter::GetDefaults(obs_data_t *settings)
{
    obs_data_set_default_double(settings, kSettingAttenLimDb, 100.0);
    obs_data_set_default_double(settings, kSettingPostFilterBeta, 0.0);
    obs_data_set_default_bool(settings, kSettingAdaptiveQueue, true);
    obs_data_set_default_string(settings, kSettingCustomModelPath, "");
    obs_data_set_default_string(settings, kSettingCustomLibraryPath, "");
}

obs_properties_t *Dfn3NoiseSuppressFilter::GetProperties(void *)
{
    obs_properties_t *props = obs_properties_create();

    obs_property_t *atten =
        obs_properties_add_float_slider(props, kSettingAttenLimDb, "Attenuation Limit (dB)", 0.0, 100.0, 1.0);
    obs_property_float_set_suffix(atten, " dB");

    obs_properties_add_float_slider(props, kSettingPostFilterBeta, "Post Filter Beta", 0.0, 0.05, 0.001);

    obs_properties_add_bool(props, kSettingAdaptiveQueue, "Enable Adaptive Queue");

    obs_properties_add_path(props, kSettingCustomModelPath, "Custom Model Path (.tar.gz)", OBS_PATH_FILE,
                            "Model Archive (*.tar.gz)", nullptr);

#ifdef _WIN32
    const char *lib_filter = "DeepFilter Library (*.dll)";
#else
    const char *lib_filter = "DeepFilter Library (*.so *.dylib)";
#endif
    obs_properties_add_path(props, kSettingCustomLibraryPath, "Custom DeepFilter Library", OBS_PATH_FILE, lib_filter,
                            nullptr);

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
    info.get_defaults = Dfn3NoiseSuppressFilter::GetDefaults;
    info.get_properties = Dfn3NoiseSuppressFilter::GetProperties;
    return info;
}();
