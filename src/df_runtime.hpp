#pragma once

#include <cstddef>
#include <string>
#include <vector>

class DeepFilterRuntime {
public:
    DeepFilterRuntime() = default;
    ~DeepFilterRuntime();

    DeepFilterRuntime(const DeepFilterRuntime &) = delete;
    DeepFilterRuntime &operator=(const DeepFilterRuntime &) = delete;

    DeepFilterRuntime(DeepFilterRuntime &&other) noexcept;
    DeepFilterRuntime &operator=(DeepFilterRuntime &&other) noexcept;

    static std::vector<std::string> DefaultLibraryCandidates();
    static void ShutdownSharedApi();

    bool Create(const std::vector<std::string> &library_candidates,
                const std::string &model_path,
                float atten_lim_db,
                const std::string &log_level,
                std::string *error);

    bool Recreate(std::string *error);

    void Destroy();

    [[nodiscard]] bool IsReady() const
    {
        return state_ != nullptr;
    }

    [[nodiscard]] size_t FrameLength() const
    {
        return frame_length_;
    }

    [[nodiscard]] bool ProcessFrame(float *input, float *output, float *out_lsnr, std::string *error);

    [[nodiscard]] bool SetAttenLim(float atten_lim_db, std::string *error);
    [[nodiscard]] bool SetPostFilterBeta(float beta, std::string *error);

    [[nodiscard]] std::string PollLogMessage();

private:
    static bool EnsureApiLoaded(const std::vector<std::string> &library_candidates, std::string *error);

    void *state_ = nullptr;
    size_t frame_length_ = 0;

    std::vector<std::string> library_candidates_;
    std::string model_path_;
    float atten_lim_db_ = 100.0f;
    std::string log_level_;
};
