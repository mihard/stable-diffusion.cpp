// Extracted from main.cpp during server refactor.

#include "async_jobs.h"

#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "common/common.h"
#include "common/log.h"
#include "common/media_io.h"
#include "common/resource_owners.hpp"

const char* async_job_kind_name(AsyncJobKind kind) {
    switch (kind) {
        case AsyncJobKind::ImgGen:
            return "img_gen";
        case AsyncJobKind::VidGen:
            return "vid_gen";
        case AsyncJobKind::Upscale:
            return "upscale";
        default:
            return "img_gen";
    }
}

const char* async_job_status_name(AsyncJobStatus status) {
    switch (status) {
        case AsyncJobStatus::Queued:
            return "queued";
        case AsyncJobStatus::Generating:
            return "generating";
        case AsyncJobStatus::Completed:
            return "completed";
        case AsyncJobStatus::Failed:
            return "failed";
        case AsyncJobStatus::Cancelled:
            return "cancelled";
        default:
            return "failed";
    }
}

void purge_expired_jobs(AsyncJobManager& manager) {
    const int64_t now = unix_timestamp_now();

    for (auto it = manager.expired_jobs.begin(); it != manager.expired_jobs.end();) {
        if (it->second <= now) {
            it = manager.expired_jobs.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = manager.jobs.begin(); it != manager.jobs.end();) {
        const auto& job = it->second;
        if (job->completed_at == 0) {
            ++it;
            continue;
        }

        int64_t ttl_seconds = job->status == AsyncJobStatus::Completed
                                  ? manager.completed_ttl_seconds
                                  : manager.failed_ttl_seconds;
        if (now - job->completed_at >= ttl_seconds) {
            manager.expired_jobs[job->id] = now + std::max<int64_t>(ttl_seconds, 60);
            it                            = manager.jobs.erase(it);
        } else {
            ++it;
        }
    }
}

size_t count_pending_jobs(const AsyncJobManager& manager) {
    size_t pending = 0;
    for (const auto& entry : manager.jobs) {
        if (entry.second->status == AsyncJobStatus::Queued ||
            entry.second->status == AsyncJobStatus::Generating) {
            ++pending;
        }
    }
    return pending;
}

std::string make_async_job_id(AsyncJobManager& manager) {
    std::ostringstream oss;
    oss << "job_" << std::hex << unix_timestamp_now() << "_" << std::setw(8)
        << std::setfill('0') << manager.next_id++;
    return oss.str();
}

bool cancel_queued_job(AsyncJobManager& manager, AsyncGenerationJob& job) {
    auto new_end = std::remove(manager.queue.begin(), manager.queue.end(), job.id);
    if (new_end == manager.queue.end()) {
        return false;
    }

    manager.queue.erase(new_end, manager.queue.end());
    job.status       = AsyncJobStatus::Cancelled;
    job.completed_at = unix_timestamp_now();
    job.result_images_b64.clear();
    job.result_media_b64.clear();
    job.result_media_mime_type.clear();
    job.result_frame_count = 0;
    job.result_fps         = 0;
    job.error_code         = "cancelled";
    job.error_message      = "job cancelled by client";
    return true;
}

json make_async_job_json(const AsyncJobManager& manager, const AsyncGenerationJob& job) {
    json result;
    result["id"]             = job.id;
    result["kind"]           = async_job_kind_name(job.kind);
    result["status"]         = async_job_status_name(job.status);
    result["created"]        = job.created_at;
    result["started"]        = job.started_at == 0 ? json(nullptr) : json(job.started_at);
    result["completed"]      = job.completed_at == 0 ? json(nullptr) : json(job.completed_at);
    result["queue_position"] = 0;

    if (job.status == AsyncJobStatus::Queued) {
        size_t position = 1;
        for (const auto& queued_id : manager.queue) {
            if (queued_id == job.id) {
                result["queue_position"] = position;
                break;
            }
            ++position;
        }
    }

    if (job.status == AsyncJobStatus::Completed) {
        if (job.kind == AsyncJobKind::VidGen) {
            result["result"] = {
                {"output_format", job.vid_gen.output_format},
                {"mime_type", job.result_media_mime_type},
                {"fps", job.result_fps},
                {"frame_count", job.result_frame_count},
                {"b64_json", job.result_media_b64},
            };
        } else {
            json images = json::array();
            for (size_t i = 0; i < job.result_images_b64.size(); ++i) {
                images.push_back({{"index", i}, {"b64_json", job.result_images_b64[i]}});
            }
            const std::string& out_fmt = (job.kind == AsyncJobKind::Upscale)
                                             ? job.upscale.output_format
                                             : job.img_gen.output_format;
            result["result"] = {
                {"output_format", out_fmt},
                {"images", images},
            };
        }
        result["error"] = nullptr;
    } else if (job.status == AsyncJobStatus::Failed ||
               job.status == AsyncJobStatus::Cancelled) {
        result["result"] = nullptr;
        result["error"]  = {
             {"code",
             job.error_code.empty()
                  ? (job.status == AsyncJobStatus::Cancelled ? "cancelled" : "generation_failed")
                  : job.error_code},
             {"message", job.error_message},
        };
    } else {
        result["result"] = nullptr;
        result["error"]  = nullptr;
    }

    return result;
}

bool execute_img_gen_job(ServerRuntime& runtime,
                         AsyncGenerationJob& job,
                         std::vector<std::string>& output_images,
                         std::string& error_message) {
    sd_img_gen_params_t params = job.img_gen.to_sd_img_gen_params_t();

    SDImageVec results;

    {
        std::lock_guard<std::mutex> lock(*runtime.sd_ctx_mutex);
        sd_image_t* raw_results = generate_image(runtime.sd_ctx, &params);
        results.adopt(raw_results, params.batch_count);
    }

    const int num_results = results.count();
    if (num_results <= 0) {
        error_message = "generate_image returned no results";
        return false;
    }

    EncodedImageFormat encoded_format = EncodedImageFormat::PNG;
    if (job.img_gen.output_format == "jpeg") {
        encoded_format = EncodedImageFormat::JPEG;
    } else if (job.img_gen.output_format == "webp") {
        encoded_format = EncodedImageFormat::WEBP;
    }

    for (int i = 0; i < num_results; ++i) {
        if (results[i].data == nullptr) {
            continue;
        }

        const std::string metadata = job.img_gen.gen_params.embed_image_metadata
                                         ? get_image_params(*runtime.ctx_params,
                                                            job.img_gen.gen_params,
                                                            job.img_gen.gen_params.seed + i)
                                         : "";
        auto image_bytes           = encode_image_to_vector(encoded_format,
                                                            results[i].data,
                                                            results[i].width,
                                                            results[i].height,
                                                            results[i].channel,
                                                            metadata,
                                                            job.img_gen.output_compression);
        if (image_bytes.empty()) {
            continue;
        }
        output_images.push_back(base64_encode(image_bytes));
    }

    if (output_images.empty()) {
        error_message = "generate_image returned empty encoded outputs";
        return false;
    }

    return true;
}

bool execute_vid_gen_job(ServerRuntime& runtime,
                         AsyncGenerationJob& job,
                         std::string& output_media_b64,
                         std::string& output_media_mime_type,
                         int& output_frame_count,
                         int& output_fps,
                         std::string& error_message) {
    sd_vid_gen_params_t params = job.vid_gen.to_sd_vid_gen_params_t();

    SDImageVec results;
    int num_results             = 0;
    sd_audio_t* generated_audio = nullptr;

    {
        std::lock_guard<std::mutex> lock(*runtime.sd_ctx_mutex);
        sd_image_t* raw_results = nullptr;
        if (!generate_video(runtime.sd_ctx, &params, &raw_results, &num_results, &generated_audio)) {
            raw_results = nullptr;
        }
        results.adopt(raw_results, num_results);
    }

    num_results = results.count();
    if (num_results <= 0) {
        free_sd_audio(generated_audio);
        error_message = "generate_video returned no results";
        return false;
    }

    std::vector<uint8_t> video_bytes = create_video_from_sd_images_to_vector(job.vid_gen.output_format,
                                                                             results.data(),
                                                                             num_results,
                                                                             job.vid_gen.gen_params.fps,
                                                                             job.vid_gen.output_compression,
                                                                             generated_audio);
    free_sd_audio(generated_audio);
    if (video_bytes.empty()) {
        error_message = "failed to encode generated video container";
        return false;
    }

    output_media_b64       = base64_encode(video_bytes);
    output_media_mime_type = video_mime_type(job.vid_gen.output_format);
    output_frame_count     = num_results;
    output_fps             = job.vid_gen.gen_params.fps;
    return true;
}

static SDImageOwner apply_upscaler(ServerRuntime& runtime,
                                    const std::string& upscaler_name,
                                    const sd_image_t& input,
                                    int target_w,
                                    int target_h,
                                    int tile_size) {
    if (upscaler_name.empty() || upscaler_name == "None") {
        // Return input as-is (copy pixels)
        size_t nbytes = static_cast<size_t>(input.width) * input.height * input.channel;
        uint8_t* buf  = static_cast<uint8_t*>(malloc(nbytes));
        if (!buf) return SDImageOwner();
        memcpy(buf, input.data, nbytes);
        return SDImageOwner({input.width, input.height, input.channel, buf});
    }

    if (upscaler_name == "Lanczos") {
        sd_image_t resized = resize_sd_image(input, target_w, target_h, true);
        return SDImageOwner(resized);
    }

    if (upscaler_name == "Nearest") {
        sd_image_t resized = resize_sd_image(input, target_w, target_h, false);
        return SDImageOwner(resized);
    }

    // Model-backed upscaler: look up path in cache
    std::string fullpath;
    {
        std::lock_guard<std::mutex> lock(*runtime.upscaler_mutex);
        for (const auto& entry : *runtime.upscaler_cache) {
            if (entry.name == upscaler_name) {
                fullpath = entry.fullpath;
                break;
            }
        }
    }
    if (fullpath.empty()) {
        LOG_ERROR("upscaler not found: %s", upscaler_name.c_str());
        return SDImageOwner();
    }

    UpscalerCtxPtr ctx(new_upscaler_ctx(fullpath.c_str(),
                                        runtime.ctx_params->offload_params_to_cpu,
                                        runtime.ctx_params->diffusion_conv_direct,
                                        runtime.ctx_params->n_threads,
                                        tile_size,
                                        runtime.ctx_params->backend.c_str(),
                                        runtime.ctx_params->params_backend.c_str()));
    if (!ctx) {
        LOG_ERROR("new_upscaler_ctx failed for: %s", fullpath.c_str());
        return SDImageOwner();
    }

    SDImageOwner upscaled(upscale(ctx.get(), input, 4));
    if (!upscaled.get().data) {
        LOG_ERROR("upscale() returned null for: %s", upscaler_name.c_str());
        return SDImageOwner();
    }

    const sd_image_t& up = upscaled.get();
    if (static_cast<int>(up.width) == target_w && static_cast<int>(up.height) == target_h) {
        return upscaled;
    }

    // Model output differs from target — resize to exact target
    sd_image_t resized = resize_sd_image(up, target_w, target_h, true);
    return SDImageOwner(resized);
}

bool perform_upscale(ServerRuntime& runtime,
                     const UpscaleJobRequest& request,
                     std::string& output_image_b64,
                     std::string& error_message) {
    refresh_upscaler_cache(runtime);

    SDImageOwner decoded_image;
    if (!decode_base64_image(request.image_b64, 3, 0, 0, decoded_image)) {
        error_message = "failed to decode input image";
        return false;
    }
    const sd_image_t& src = decoded_image.get();

    const int tw = (request.target_width > 0)
                       ? request.target_width
                       : static_cast<int>(std::round(src.width * request.scale));
    const int th = (request.target_height > 0)
                       ? request.target_height
                       : static_cast<int>(std::round(src.height * request.scale));

    if (tw <= 0 || th <= 0) {
        error_message = "computed target dimensions are invalid";
        return false;
    }

    SDImageOwner image_1 = apply_upscaler(runtime, request.upscaler_1, src, tw, th, request.tile_size);
    if (!image_1.get().data) {
        error_message = "upscaler_1 failed ('" + request.upscaler_1 + "')";
        return false;
    }

    const bool use_blend = (request.upscaler_2 != "None" && !request.upscaler_2.empty() &&
                             request.upscaler_2_visibility > 0.0f);
    if (use_blend) {
        SDImageOwner image_2 = apply_upscaler(runtime, request.upscaler_2, src, tw, th, request.tile_size);
        if (!image_2.get().data) {
            error_message = "upscaler_2 failed ('" + request.upscaler_2 + "')";
            return false;
        }

        const sd_image_t& i1 = image_1.get();
        const sd_image_t& i2 = image_2.get();
        const float v         = std::min(std::max(request.upscaler_2_visibility, 0.0f), 1.0f);
        const size_t npixels  = static_cast<size_t>(tw) * th * i1.channel;
        for (size_t i = 0; i < npixels; ++i) {
            float blended = i1.data[i] * (1.0f - v) + i2.data[i] * v;
            i1.data[i]    = static_cast<uint8_t>(std::min(std::max(blended + 0.5f, 0.0f), 255.0f));
        }
    }

    EncodedImageFormat fmt = EncodedImageFormat::PNG;
    if (request.output_format == "jpeg") {
        fmt = EncodedImageFormat::JPEG;
    } else if (request.output_format == "webp") {
        fmt = EncodedImageFormat::WEBP;
    }

    const sd_image_t& out = image_1.get();
    auto encoded = encode_image_to_vector(fmt, out.data, out.width, out.height, out.channel,
                                          "", request.output_compression);
    if (encoded.empty()) {
        error_message = "failed to encode output image";
        return false;
    }

    output_image_b64 = base64_encode(encoded);
    return true;
}

bool execute_upscale_job(ServerRuntime& runtime,
                         AsyncGenerationJob& job,
                         std::string& output_image_b64,
                         std::string& error_message) {
    return perform_upscale(runtime, job.upscale, output_image_b64, error_message);
}

void async_job_worker(ServerRuntime& runtime) {
    AsyncJobManager& manager = *runtime.async_job_manager;

    while (true) {
        std::shared_ptr<AsyncGenerationJob> job;
        {
            std::unique_lock<std::mutex> lock(manager.mutex);
            manager.cv.wait(lock, [&]() { return manager.stop || !manager.queue.empty(); });

            if (manager.stop && manager.queue.empty()) {
                break;
            }

            purge_expired_jobs(manager);
            if (manager.queue.empty()) {
                continue;
            }

            const std::string job_id = manager.queue.front();
            manager.queue.pop_front();

            auto it = manager.jobs.find(job_id);
            if (it == manager.jobs.end()) {
                continue;
            }

            job             = it->second;
            job->status     = AsyncJobStatus::Generating;
            job->started_at = unix_timestamp_now();
        }

        std::vector<std::string> output_images;
        std::string output_media_b64;
        std::string output_media_mime_type;
        int output_frame_count = 0;
        int output_fps         = 0;
        std::string error_message;
        bool ok = false;

        if (job->kind == AsyncJobKind::ImgGen) {
            ok = execute_img_gen_job(runtime, *job, output_images, error_message);
        } else if (job->kind == AsyncJobKind::VidGen) {
            ok = execute_vid_gen_job(runtime,
                                     *job,
                                     output_media_b64,
                                     output_media_mime_type,
                                     output_frame_count,
                                     output_fps,
                                     error_message);
        } else if (job->kind == AsyncJobKind::Upscale) {
            std::string upscale_b64;
            ok = execute_upscale_job(runtime, *job, upscale_b64, error_message);
            if (ok) {
                output_images.push_back(std::move(upscale_b64));
            }
        } else {
            error_message = "unsupported job kind";
        }

        {
            std::lock_guard<std::mutex> lock(manager.mutex);
            auto it = manager.jobs.find(job->id);
            if (it == manager.jobs.end()) {
                continue;
            }

            job->completed_at = unix_timestamp_now();
            if (ok) {
                job->status                 = AsyncJobStatus::Completed;
                job->result_images_b64      = std::move(output_images);
                job->result_media_b64       = std::move(output_media_b64);
                job->result_media_mime_type = std::move(output_media_mime_type);
                job->result_frame_count     = output_frame_count;
                job->result_fps             = output_fps;
                job->error_code.clear();
                job->error_message.clear();
            } else {
                job->status        = AsyncJobStatus::Failed;
                job->error_code    = "generation_failed";
                job->error_message = error_message.empty() ? "unknown generation error" : error_message;
                job->result_images_b64.clear();
                job->result_media_b64.clear();
                job->result_media_mime_type.clear();
                job->result_frame_count = 0;
                job->result_fps         = 0;
            }

            purge_expired_jobs(manager);
        }
    }
}
