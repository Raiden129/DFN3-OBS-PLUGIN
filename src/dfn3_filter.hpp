#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <media-io/audio-resampler.h>
#include <obs-module.h>
}

#include "df_runtime.hpp"
#include "ring_buffer.hpp"

class Dfn3NoiseSuppressFilter {
public:
    static constexpr const char *kFilterId = "dfn3_noise_suppress_filter";

    explicit Dfn3NoiseSuppressFilter(obs_source_t *context);
    ~Dfn3NoiseSuppressFilter();

    Dfn3NoiseSuppressFilter(const Dfn3NoiseSuppressFilter &) = delete;
    Dfn3NoiseSuppressFilter &operator=(const Dfn3NoiseSuppressFilter &) = delete;

    void Update(obs_data_t *settings);
    obs_audio_data *FilterAudio(obs_audio_data *audio);

    static const char *GetName(void *);
    static void *Create(obs_data_t *settings, obs_source_t *context);
    static void Destroy(void *data);
    static void UpdateCallback(void *data, obs_data_t *settings);
    static obs_audio_data *FilterAudioCallback(void *data, obs_audio_data *audio);
    static void ActivateCallback(void *data);
    static void DeactivateCallback(void *data);
    static void GetDefaults(obs_data_t *settings);
    static obs_properties_t *GetProperties(void *data);

private:
    struct Settings {
        float atten_lim_db = 100.0f;
        float post_filter_beta = 0.0f;
        bool adaptive_queue = true;
        bool show_debug_details = false;
    };

    struct PacketInfo {
        uint32_t frames = 0;
        uint64_t timestamp_ns = 0;
        uint64_t input_ts_offset_ns = 0;
    };

    static constexpr uint32_t kModelSampleRate = 48000;
    static constexpr size_t kMaxChannels = 2;
    static constexpr size_t kMaxInputQueueHops = 4;
    static constexpr size_t kMaxOutputQueueHops = 4;
    static constexpr size_t kMinQueueHops = 1;
    static constexpr size_t kMaxPacketQueueEntries = 64;
    static constexpr size_t kMaxExpectedCallbackFrames = 2048;
    static constexpr uint64_t kTimestampResetThresholdNs = 1000000000ULL;

    void StartWorker();
    void StopWorker();
    void WorkerLoop();

    bool EnsureConfiguredForStream(uint32_t sample_rate, size_t channels);
    bool RebuildRuntimeLocked();
    bool ReconfigureBuffersLocked(uint32_t sample_rate, size_t channels, size_t hop_size);
    void DestroyResamplersLocked();
    bool CreateResamplersLocked(uint32_t sample_rate, size_t channels);
    void FlushQueuesLocked();
    bool DropOldestInputHopLocked();
    bool EnsureInputWriteCapacityLocked(size_t required_samples, size_t *dropped_hops);

    bool PushInputLocked(const obs_audio_data *audio, uint64_t *input_ts_offset_ns);
    bool PushInputFromResamplerLocked(const obs_audio_data *audio, uint64_t *input_ts_offset_ns);

    bool ProcessOneHop();
    bool PopPacketToOutput(const PacketInfo &packet, obs_audio_data *target);
    bool PrepareHostOutputLocked(size_t needed_frames, size_t *queue_dwell_frames);

    bool PushPacketLocked(const PacketInfo &packet);
    bool PeekPacketLocked(PacketInfo *packet) const;
    void PopPacketLocked();

    uint64_t ComputeLatencyNs(size_t queue_dwell_frames,
                              uint64_t input_ts_offset_ns,
                              uint64_t output_ts_offset_ns) const;
    uint64_t ModelDelaySamples() const;

    std::string ResolveModelPath() const;
    std::vector<std::string> BuildLibraryCandidates() const;
    std::string BuildStatusText(bool show_debug_details) const;
    bool ShowDebugDetailsEnabled() const;
    void Tick(float seconds);
    static void TickCallback(void *param, float seconds);
    static bool DebugDetailsModified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings);
    void SetLastError(const std::string &error);
    void ClearLastError();
    std::string GetLastError() const;

    static enum speaker_layout LayoutFromChannels(size_t channels);

    void RequestRebuild();
    void ApplyRuntimeControls();
    void Activate();
    void Deactivate();

    obs_source_t *context_ = nullptr;

    std::thread worker_thread_;
    std::atomic<bool> stop_worker_{false};

    mutable std::mutex config_mutex_;
    mutable std::mutex input_mutex_;
    mutable std::mutex output_mutex_;
    mutable std::mutex packet_mutex_;
    mutable std::mutex runtime_access_mutex_;
    std::condition_variable input_cv_;

    Settings settings_;
    bool rebuild_requested_ = true;
    std::atomic<bool> runtime_ready_{false};
    std::atomic<bool> controls_dirty_{true};

    uint32_t sample_rate_ = 0;
    size_t channels_ = 0;
    size_t hop_size_ = 480;
    std::atomic<size_t> queue_hops_{kMinQueueHops};

    uint64_t last_timestamp_ns_ = 0;

    std::vector<DeepFilterRuntime> runtimes_;

    std::vector<SampleRingBuffer> input_queues_48k_;
    std::vector<SampleRingBuffer> output_queues_48k_;
    std::vector<SampleRingBuffer> host_output_queues_;
    std::array<PacketInfo, kMaxPacketQueueEntries> packet_queue_{};
    size_t packet_queue_head_ = 0;
    size_t packet_queue_size_ = 0;

    audio_resampler_t *resampler_to_model_ = nullptr;
    audio_resampler_t *resampler_from_model_ = nullptr;

    std::vector<std::vector<float>> hop_input_scratch_;
    std::vector<std::vector<float>> hop_output_scratch_;
    std::vector<std::vector<float>> resample_hop_scratch_;
    std::vector<float> mono_input_scratch_;
    std::vector<float> mono_output_scratch_;
    std::atomic<uint64_t> resampler_from_model_ts_offset_ns_{0};

    std::atomic<uint64_t> overflow_count_{0};
    std::atomic<uint64_t> underrun_count_{0};
    std::atomic<uint64_t> reset_count_{0};

    std::atomic<double> rolling_queue_depth_sum_{0.0};
    std::atomic<uint64_t> rolling_queue_depth_count_{0};

    std::string last_error_;

    double status_refresh_accum_s_ = 0.0;
    std::string last_published_status_text_;
    bool last_published_status_valid_ = false;
};

extern struct obs_source_info dfn3_noise_suppress_filter_info;
