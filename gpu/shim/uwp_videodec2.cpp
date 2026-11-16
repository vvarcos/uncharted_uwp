// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: the video decoder, reduced to the two entry points this build needs.
//
// libraries/videodec/videodec2.cpp is the full implementation, but it pulls FFmpeg (libavcodec) and this
// UWP build does not link it. The boot only asks for the compute-memory query and the compute queue, and
// neither decodes anything: the query is a pure struct fill and the queue is bookkeeping. Registering
// only these two keeps the decoder visible to the guest without dragging a decoder into the build.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common/types.h"
#include "core/libraries/libs.h"
#include "core/libraries/videodec/videodec2.h"
#include "core/libraries/videodec/videodec_error.h"

namespace {

using namespace Libraries::Videodec2;

constexpr u64 kMinimumMemorySize = 16_MB;
OrbisVideodec2DecoderConfigInfo s_decoder_config{};

s32 PS4_SYSV_ABI QueryComputeMemoryInfo(OrbisVideodec2ComputeMemoryInfo* info) {
    if (info == nullptr) {
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if (info->this_size != sizeof(OrbisVideodec2ComputeMemoryInfo)) {
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }
    info->cpu_gpu_memory = nullptr;
    info->cpu_gpu_memory_size = kMinimumMemorySize;
    std::printf("M5 videodec2: QueryComputeMemoryInfo size=%llu\n",
                static_cast<unsigned long long>(kMinimumMemorySize));
    std::fflush(stdout);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI AllocateComputeQueue(const OrbisVideodec2ComputeConfigInfo* cfg,
                                      const OrbisVideodec2ComputeMemoryInfo* memory,
                                      OrbisVideodec2ComputeQueue* queue) {
    if (cfg == nullptr || memory == nullptr || queue == nullptr) {
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    /* The queue is an opaque void* and the guest dereferences it, so returning an error here and leaving
     * the pointer untouched made it read -1 (measured: fault at 0xffff...ff). Answer success with a
     * zeroed block: every field the guest may read says "nothing here", which is the only honest value
     * for a decoder this build does not have. */
    static u8 s_queue[4096]{};
    *queue = s_queue;
    std::printf("M5 videodec2: AllocateComputeQueue -> OK (zeroed stub queue)\n");
    std::fflush(stdout);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI QueryDecoderMemoryInfo(const OrbisVideodec2DecoderConfigInfo* cfg,
                                       OrbisVideodec2DecoderMemoryInfo* info) {
    if (cfg == nullptr || info == nullptr) {
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    if (cfg->this_size != sizeof(OrbisVideodec2DecoderConfigInfo) ||
        info->this_size != sizeof(OrbisVideodec2DecoderMemoryInfo)) {
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }
    /* Mirrors the reference implementation's degenerate path (it takes it whenever the frame size
     * cannot be derived): every buffer is the minimum, no pointer is handed out, alignment 0x100. */
    info->cpu_memory = nullptr;
    info->gpu_memory = nullptr;
    info->cpu_gpu_memory = nullptr;
    info->cpu_memory_size = kMinimumMemorySize;
    info->gpu_memory_size = kMinimumMemorySize;
    info->cpu_gpu_memory_size = kMinimumMemorySize;
    info->max_frame_buffer_size = kMinimumMemorySize;
    info->frame_buffer_alignment = 0x100;
    std::printf("M5 videodec2: QueryDecoderMemoryInfo -> OK (%llu bytes each)\n",
                static_cast<unsigned long long>(kMinimumMemorySize));
    std::fflush(stdout);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Decode(OrbisVideodec2Decoder decoder, const OrbisVideodec2InputData* input,
                        OrbisVideodec2FrameBuffer* frame, OrbisVideodec2OutputInfo* output) {
    /* No decoder in this build. The question that matters is whether the movie path gives up or retries,
     * so the access unit is logged: if the pointer advances the guest is consuming the stream, if it
     * repeats the guest is retrying the same unit and the answer has to change. */
    static int logged;
    static int total;
    ++total;
    if (logged < 8 && input != nullptr) {
        ++logged;
        std::printf("M5 videodec2: Decode au=%p size=%llu pts=%llu dts=%llu\n", input->au_data,
                    static_cast<unsigned long long>(input->au_size),
                    static_cast<unsigned long long>(input->pts_data),
                    static_cast<unsigned long long>(input->dts_data));
        std::fflush(stdout);
    }
    if (total == 64) {
        std::printf("M5 videodec2: Decode total=%d (log capped)\n", total);
        std::fflush(stdout);
    }
    (void)decoder;
    if (frame == nullptr || output == nullptr || input == nullptr) {
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    frame->is_accepted = true;
    const u64 this_size = output->this_size;
    std::memset(output, 0, sizeof(*output));
    output->this_size = this_size;
    const u32 width = (static_cast<u32>(s_decoder_config.max_frame_width) + 15u) & ~15u;
    const u32 height = (static_cast<u32>(s_decoder_config.max_frame_height) + 15u) & ~15u;
    const u32 pitch = (width + 255u) & ~255u;
    const u64 pixels_size = (static_cast<u64>(pitch) * height * 3) / 2;
    if (frame->frame_buffer != nullptr &&
        frame->frame_buffer_size >= pixels_size + sizeof(OrbisVideodec2AvcPictureInfo)) {
        std::memset(frame->frame_buffer, 0, static_cast<size_t>(pixels_size));
        output->is_valid = true;
        // The UWP stub has no parsed SPS/picture metadata. Expose the accepted frame without
        // advertising picture info; otherwise the guest enters an ABI path requiring real decoder data.
        output->picture_count = 0;
        output->codec_type = s_decoder_config.codec_type;
        output->frame_width = width;
        output->frame_pitch = pitch;
        output->frame_height = height;
        output->frame_buffer = frame->frame_buffer;
        output->frame_buffer_size = pixels_size;
        output->frame_format = 0;
        output->frame_pitch_in_bytes = pitch;
        auto* picture = reinterpret_cast<OrbisVideodec2AvcPictureInfo*>(
            static_cast<u8*>(frame->frame_buffer) + pixels_size);
        std::memset(picture, 0, sizeof(*picture));
        picture->is_valid = true;
        picture->pts_data = input->pts_data;
        picture->dts_data = input->dts_data;
        picture->attached_data = input->attached_data;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI CreateDecoder(const OrbisVideodec2DecoderConfigInfo* cfg,
                               const OrbisVideodec2DecoderMemoryInfo* memory,
                               OrbisVideodec2Decoder* decoder) {
    /* The library rule measured here: the caller checks the handle, not the return code. Answering
     * success without a handle left it calling a pointer holding -1, so this hands out a static non-null
     * instance and lets Decode report the stream failure the guest already tolerates. */
    if (cfg != nullptr) s_decoder_config = *cfg;
    (void)memory;
    static u8 s_decoder[4096]{};
    if (decoder != nullptr) {
        *decoder = reinterpret_cast<OrbisVideodec2Decoder>(s_decoder);
    }
    std::printf("M5 videodec2: CreateDecoder -> OK codec=%u max=%dx%d (static handle)\n",
                static_cast<u32>(s_decoder_config.codec_type), s_decoder_config.max_frame_width,
                s_decoder_config.max_frame_height);
    std::fflush(stdout);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI ResetDecoder(OrbisVideodec2Decoder decoder) {
    (void)decoder;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI ReleaseComputeQueue(OrbisVideodec2ComputeQueue queue) {
    (void)queue;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI DeleteDecoder(OrbisVideodec2Decoder decoder) {
    (void)decoder;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Flush(OrbisVideodec2Decoder decoder, OrbisVideodec2FrameBuffer* frame,
                       OrbisVideodec2OutputInfo* output) {
    (void)decoder;
    if (frame != nullptr) frame->is_accepted = true;
    if (output != nullptr) {
        const u64 this_size = output->this_size;
        std::memset(output, 0, sizeof(*output));
        output->this_size = this_size;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI GetPictureInfo(const OrbisVideodec2OutputInfo* output, void* first,
                                void* second) {
    if (output == nullptr || first == nullptr || !output->is_valid || output->frame_buffer == nullptr) {
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    const auto* picture = reinterpret_cast<const OrbisVideodec2AvcPictureInfo*>(
        static_cast<const u8*>(output->frame_buffer) + output->frame_buffer_size);
    const u64 requested_size = *static_cast<const u64*>(first);
    static int size_logged;
    if (size_logged++ < 4) {
        std::printf("M5 videodec2: GetPictureInfo dst=%p requested=%llu expected=%llu\n", first,
                    static_cast<unsigned long long>(requested_size),
                    static_cast<unsigned long long>(sizeof(*picture)));
        std::fflush(stdout);
    }
    if (requested_size < sizeof(u64)) {
        return ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE;
    }
    const u64 copy_size = std::min<u64>(requested_size, sizeof(*picture));
    std::memcpy(static_cast<u8*>(first) + sizeof(u64),
                reinterpret_cast<const u8*>(picture) + sizeof(u64),
                static_cast<size_t>(copy_size - sizeof(u64)));
    (void)second;
    static int logged;
    if (logged++ < 4) {
        std::printf("M5 videodec2: GetPictureInfo pts=%llu\n",
                    static_cast<unsigned long long>(picture->pts_data));
        std::fflush(stdout);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI GetHevcPictureInfo(const OrbisVideodec2OutputInfo* output,
                                    OrbisVideodec2HevcPictureInfo* picture) {
    if (output == nullptr || picture == nullptr || !output->is_valid ||
        output->frame_buffer == nullptr) {
        return ORBIS_VIDEODEC2_ERROR_ARGUMENT_POINTER;
    }
    const auto* stored = reinterpret_cast<const OrbisVideodec2HevcPictureInfo*>(
        static_cast<const u8*>(output->frame_buffer) + output->frame_buffer_size);
    std::memcpy(picture, stored, sizeof(*picture));
    return ORBIS_OK;
}

} // namespace

namespace Libraries::Videodec2 {
void RegisterUwpMinimal(Core::Loader::SymbolsResolver* sym);
}

void Libraries::Videodec2::RegisterUwpMinimal(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("RnDibcGCPKw", "libSceVideodec2", 1, "libSceVideodec2", QueryComputeMemoryInfo);
    LIB_FUNCTION("eD+X2SmxUt4", "libSceVideodec2", 1, "libSceVideodec2", AllocateComputeQueue);
    LIB_FUNCTION("UvtA3FAiF4Y", "libSceVideodec2", 1, "libSceVideodec2", ReleaseComputeQueue);
    /* The boot asks for this immediately after the queue, and leaving it unresolved made the guest call
     * a function pointer holding -1 (measured: call rax with rax = 0xffff...ff). */
    LIB_FUNCTION("qqMCwlULR+E", "libSceVideodec2", 1, "libSceVideodec2", QueryDecoderMemoryInfo);
    /* Experiment (a): the caller checks the handle, not the return code, so CreateDecoder answers OK with
     * a static non-null handle and Decode keeps failing the stream the guest already accepted. */
    LIB_FUNCTION("CNNRoRYd8XI", "libSceVideodec2", 1, "libSceVideodec2", CreateDecoder);
    LIB_FUNCTION("jwImxXRGSKA", "libSceVideodec2", 1, "libSceVideodec2", DeleteDecoder);
    LIB_FUNCTION("852F5+q6+iM", "libSceVideodec2", 1, "libSceVideodec2", Decode);
    LIB_FUNCTION("l1hXwscLuCY", "libSceVideodec2", 1, "libSceVideodec2", Flush);
    LIB_FUNCTION("wJXikG6QFN8", "libSceVideodec2", 1, "libSceVideodec2", ResetDecoder);
    LIB_FUNCTION("NtXRa3dRzU0", "libSceVideodec2", 1, "libSceVideodec2", GetPictureInfo);
    LIB_FUNCTION("kjrLbcyhEiw", "libSceVideodec2", 1, "libSceVideodec2", GetPictureInfo);
    LIB_FUNCTION("7M+1UFqWOAI", "libSceVideodec2", 1, "libSceVideodec2", GetHevcPictureInfo);
}
