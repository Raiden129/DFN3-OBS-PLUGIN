#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
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
    static void GetDefaults(obs_data_t *settings);
    static obs_properties_t *GetProperties(void *data);

private:
    struct Settings {
        float atten_lim_db = 100.0f;
        float post_filter_beta = 0.0f;
        bool adaptive_queue = true;
        std::string custom_model_path;
        std::string custom_library_path;
    };

    struct PacketInfo {
        uint32_t frames = 0;
        uint64_t timestamp_ns = 0;
    };

    static constexpr uint32_t kModelSampleRate = 48000;
    static constexpr size_t kMaxChannels = 2;
    static constexpr size_t kMaxInputQueueHops = 4;
    static constexpr size_t kMaxOutputQueueHops = 4;
    static constexpr size_t kMinQueueHops = 1;
    static constexpr uint64_t kTimestampResetThresholdNs = 1000000000ULL;

    void StartWorker();
    void StopWorker();
    void WorkerLoop();

    bool EnsureConfiguredForStream(uint32_t sample_rate, size_t channels);
    bool RebuildRuntimeLocked();
    void ReconfigureBuffersLocked(uint32_t sample_rate, size_t channels, size_t hop_size);
    void DestroyResamplersLocked();
    bool CreateResamplersLocked(uint32_t sample_rate, size_t channels);
    void FlushQueuesLocked();

    bool PushInputLocked(const obs_audio_data *audio);
    bool PushInputFromResamplerLocked(const obs_audio_data *audio);

    bool ProcessOneHop();
    bool PopPacketToOutput(const PacketInfo &packet);
    bool PrepareHostOutputLocked(size_t needed_frames);

    uint64_t ComputeLatencyNs() const;
    uint64_t ModelDelaySamples() const;

    std::string ResolveModelPath(const Settings &settings) const;
    std::vector<std::string> BuildLibraryCandidates(const Settings &settings) const;

    static enum speaker_layout LayoutFromChannels(size_t channels);

    void RequestRebuild();
    void ApplyRuntimeControls();

    obs_source_t *context_ = nullptr;

    std::thread worker_thread_;
    std::atomic<bool> stop_worker_{false};

    mutable std::mutex config_mutex_;
    mutable std::mutex input_mutex_;
    mutable std::mutex output_mutex_;
    std::condition_variable input_cv_;

    Settings settings_;
    bool rebuild_requested_ = true;
    std::atomic<bool> runtime_ready_{false};

    uint32_t sample_rate_ = 0;
    size_t channels_ = 0;
    size_t hop_size_ = 480;
    std::atomic<size_t> queue_hops_{kMinQueueHops};

    uint64_t last_timestamp_ns_ = 0;

    std::vector<DeepFilterRuntime> runtimes_;

    std::vector<SampleRingBuffer> input_queues_48k_;
    std::vector<SampleRingBuffer> output_queues_48k_;
    std::vector<SampleRingBuffer> host_output_queues_;
    std::deque<PacketInfo> packet_queue_;

    audio_resampler_t *resampler_to_model_ = nullptr;
    audio_resampler_t *resampler_from_model_ = nullptr;

    std::vector<std::vector<float>> hop_input_scratch_;
    std::vector<std::vector<float>> hop_output_scratch_;
    std::vector<std::vector<float>> resample_hop_scratch_;

    std::vector<float> output_storage_;
    obs_audio_data output_audio_{};

    std::atomic<uint64_t> overflow_count_{0};
    std::atomic<uint64_t> underrun_count_{0};
    std::atomic<uint64_t> reset_count_{0};

    std::atomic<double> rolling_queue_depth_sum_{0.0};
    std::atomic<uint64_t> rolling_queue_depth_count_{0};

    std::string last_error_;
};

extern struct obs_source_info dfn3_noise_suppress_filter_info;
