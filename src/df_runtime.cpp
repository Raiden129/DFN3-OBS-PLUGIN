#include "df_runtime.hpp"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

using df_create_fn = void *(*)(const char *path, float atten_lim, const char *log_level);
using df_get_frame_length_fn = size_t (*)(void *st);
using df_process_frame_fn = float (*)(void *st, float *input, float *output);
using df_set_atten_lim_fn = void (*)(void *st, float lim_db);
using df_set_post_filter_beta_fn = void (*)(void *st, float beta);
using df_next_log_msg_fn = char *(*)(void *st);
using df_free_log_msg_fn = void (*)(char *ptr);
using df_free_fn = void (*)(void *model);

struct DeepFilterApi {
#ifdef _WIN32
    HMODULE library = nullptr;
#else
    void *library = nullptr;
#endif
    std::string loaded_from;

    df_create_fn df_create = nullptr;
    df_get_frame_length_fn df_get_frame_length = nullptr;
    df_process_frame_fn df_process_frame = nullptr;
    df_set_atten_lim_fn df_set_atten_lim = nullptr;
    df_set_post_filter_beta_fn df_set_post_filter_beta = nullptr;
    df_next_log_msg_fn df_next_log_msg = nullptr;
    df_free_log_msg_fn df_free_log_msg = nullptr;
    df_free_fn df_free = nullptr;

    bool ready = false;
};

DeepFilterApi &MutableApi()
{
    static DeepFilterApi api;
    return api;
}

std::mutex &ApiMutex()
{
    static std::mutex m;
    return m;
}

#ifdef _WIN32
void *LoadSymbol(HMODULE library, const char *name)
{
    return reinterpret_cast<void *>(GetProcAddress(library, name));
}
#else
void *LoadSymbol(void *library, const char *name)
{
    return dlsym(library, name);
}
#endif

std::string PlatformLibraryName()
{
#ifdef _WIN32
    return "deepfilter.dll";
#elif __APPLE__
    return "libdeepfilter.dylib";
#else
    return "libdeepfilter.so";
#endif
}

std::vector<std::string> BuildLibraryCandidates(const std::vector<std::string> &requested)
{
    std::vector<std::string> candidates;
    candidates.reserve(requested.size() + 3);

    for (const auto &v : requested) {
        if (!v.empty()) {
            candidates.push_back(v);
        }
    }

    const auto platform_name = PlatformLibraryName();
    if (std::find(candidates.begin(), candidates.end(), platform_name) == candidates.end()) {
        candidates.push_back(platform_name);
    }

#ifdef _WIN32
    if (std::find(candidates.begin(), candidates.end(), std::string("deep_filter.dll")) == candidates.end()) {
        candidates.emplace_back("deep_filter.dll");
    }
    if (std::find(candidates.begin(), candidates.end(), std::string("libdeepfilter.dll")) == candidates.end()) {
        candidates.emplace_back("libdeepfilter.dll");
    }
#endif

    return candidates;
}

bool ResolveSymbols(DeepFilterApi &api)
{
    api.df_create = reinterpret_cast<df_create_fn>(LoadSymbol(api.library, "df_create"));
    api.df_get_frame_length =
        reinterpret_cast<df_get_frame_length_fn>(LoadSymbol(api.library, "df_get_frame_length"));
    api.df_process_frame = reinterpret_cast<df_process_frame_fn>(LoadSymbol(api.library, "df_process_frame"));
    api.df_set_atten_lim = reinterpret_cast<df_set_atten_lim_fn>(LoadSymbol(api.library, "df_set_atten_lim"));
    api.df_set_post_filter_beta =
        reinterpret_cast<df_set_post_filter_beta_fn>(LoadSymbol(api.library, "df_set_post_filter_beta"));
    api.df_next_log_msg = reinterpret_cast<df_next_log_msg_fn>(LoadSymbol(api.library, "df_next_log_msg"));
    api.df_free_log_msg = reinterpret_cast<df_free_log_msg_fn>(LoadSymbol(api.library, "df_free_log_msg"));
    api.df_free = reinterpret_cast<df_free_fn>(LoadSymbol(api.library, "df_free"));

    return api.df_create && api.df_get_frame_length && api.df_process_frame && api.df_set_atten_lim &&
           api.df_set_post_filter_beta && api.df_free;
}

void CloseLibraryIfNeeded(DeepFilterApi &api)
{
    if (!api.library) {
        return;
    }
#ifdef _WIN32
    FreeLibrary(api.library);
#else
    dlclose(api.library);
#endif
    api.library = nullptr;
    api.ready = false;
    api.loaded_from.clear();
}

std::string BuildApiLoadError(const std::vector<std::string> &candidates)
{
    std::ostringstream os;
    os << "Could not load deepfilter library from any candidate path: ";
    for (size_t i = 0; i < candidates.size(); ++i) {
        os << candidates[i];
        if (i + 1 < candidates.size()) {
            os << ", ";
        }
    }
    return os.str();
}

} // namespace

DeepFilterRuntime::~DeepFilterRuntime()
{
    Destroy();
}

DeepFilterRuntime::DeepFilterRuntime(DeepFilterRuntime &&other) noexcept
{
    *this = std::move(other);
}

DeepFilterRuntime &DeepFilterRuntime::operator=(DeepFilterRuntime &&other) noexcept
{
    if (this == &other) {
        return *this;
    }

    Destroy();

    state_ = other.state_;
    frame_length_ = other.frame_length_;
    library_candidates_ = std::move(other.library_candidates_);
    model_path_ = std::move(other.model_path_);
    atten_lim_db_ = other.atten_lim_db_;
    log_level_ = std::move(other.log_level_);

    other.state_ = nullptr;
    other.frame_length_ = 0;
    other.atten_lim_db_ = 100.0f;

    return *this;
}

std::vector<std::string> DeepFilterRuntime::DefaultLibraryCandidates()
{
    return {PlatformLibraryName()};
}

bool DeepFilterRuntime::EnsureApiLoaded(const std::vector<std::string> &library_candidates, std::string *error)
{
    std::lock_guard<std::mutex> lock(ApiMutex());
    auto &api = MutableApi();

    if (api.ready) {
        return true;
    }

    const auto candidates = BuildLibraryCandidates(library_candidates);

    for (const auto &candidate : candidates) {
#ifdef _WIN32
        HMODULE module = LoadLibraryA(candidate.c_str());
        if (!module) {
            continue;
        }
        api.library = module;
#else
        void *module = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            continue;
        }
        api.library = module;
#endif

        if (ResolveSymbols(api)) {
            api.ready = true;
            api.loaded_from = candidate;
            return true;
        }

        CloseLibraryIfNeeded(api);
    }

    if (error) {
        *error = BuildApiLoadError(candidates);
    }
    return false;
}

bool DeepFilterRuntime::Create(const std::vector<std::string> &library_candidates,
                               const std::string &model_path,
                               float atten_lim_db,
                               const std::string &log_level,
                               std::string *error)
{
    Destroy();

    if (model_path.empty()) {
        if (error) {
            *error = "Model path is empty.";
        }
        return false;
    }

    if (!EnsureApiLoaded(library_candidates, error)) {
        return false;
    }

    auto &api = MutableApi();
    const char *log_level_cstr = log_level.empty() ? nullptr : log_level.c_str();
    state_ = api.df_create(model_path.c_str(), atten_lim_db, log_level_cstr);
    if (!state_) {
        if (error) {
            *error = "df_create failed and returned null state.";
        }
        return false;
    }

    frame_length_ = api.df_get_frame_length(state_);
    if (frame_length_ == 0) {
        if (error) {
            *error = "df_get_frame_length returned zero.";
        }
        Destroy();
        return false;
    }

    library_candidates_ = library_candidates;
    model_path_ = model_path;
    atten_lim_db_ = atten_lim_db;
    log_level_ = log_level;
    return true;
}

bool DeepFilterRuntime::Recreate(std::string *error)
{
    return Create(library_candidates_, model_path_, atten_lim_db_, log_level_, error);
}

void DeepFilterRuntime::Destroy()
{
    if (!state_) {
        return;
    }

    auto &api = MutableApi();
    if (api.df_free) {
        api.df_free(state_);
    }

    state_ = nullptr;
    frame_length_ = 0;
}

bool DeepFilterRuntime::ProcessFrame(const float *input, float *output, float *out_lsnr, std::string *error) const
{
    if (!state_ || !input || !output) {
        if (error) {
            *error = "Invalid runtime state or null buffers in ProcessFrame.";
        }
        return false;
    }

    auto &api = MutableApi();
    float *mutable_input = const_cast<float *>(input);
    const float lsnr = api.df_process_frame(state_, mutable_input, output);
    if (out_lsnr) {
        *out_lsnr = lsnr;
    }
    return true;
}

bool DeepFilterRuntime::SetAttenLim(float atten_lim_db, std::string *error) const
{
    if (!state_) {
        if (error) {
            *error = "Cannot set attenuation limit on uninitialized runtime.";
        }
        return false;
    }

    auto &api = MutableApi();
    api.df_set_atten_lim(state_, atten_lim_db);
    return true;
}

bool DeepFilterRuntime::SetPostFilterBeta(float beta, std::string *error) const
{
    if (!state_) {
        if (error) {
            *error = "Cannot set post filter beta on uninitialized runtime.";
        }
        return false;
    }

    auto &api = MutableApi();
    api.df_set_post_filter_beta(state_, beta);
    return true;
}

std::string DeepFilterRuntime::PollLogMessage() const
{
    if (!state_) {
        return {};
    }

    auto &api = MutableApi();
    if (!api.df_next_log_msg || !api.df_free_log_msg) {
        return {};
    }

    char *msg = api.df_next_log_msg(state_);
    if (!msg) {
        return {};
    }

    std::string result(msg);
    api.df_free_log_msg(msg);
    return result;
}
