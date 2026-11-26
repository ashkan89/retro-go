#include <chrono>
#include "nestopia_replay.hpp"

static uint64_t benchmark_hash(uint64_t hash, const void *data, size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}

int main(int argc, char **argv)
{
    using namespace Nes;
    using Clock = std::chrono::steady_clock;
    assert(argc == 7);
    NestopiaReplay replay;
    if (FILE *file = fopen(argv[2], "r"))
    {
        assert(replay.load(file));
        fclose(file);
    }
    else
        assert(!strcmp(argv[2], "-"));

    Api::Emulator emulator;
    Api::Machine machine(emulator);
    Api::Fds fds(emulator);
    Api::Video video(emulator);
    Api::Sound sound(emulator);
    if (strcmp(argv[3], "-"))
    {
        std::ifstream bios(argv[3], std::ios::binary);
        assert(bios && fds.SetBIOS(&bios) >= RESULT_OK);
    }
    assert(load_image(machine, argv[1]) >= RESULT_OK);
    assert(!machine.Is(Api::Machine::SOUND));
    assert(machine.SetMode(machine.GetDesiredMode()) >= RESULT_OK);
    Api::Input(emulator).AutoSelectControllers();
    assert(video.EnableUnlimSprites(!atoi(argv[4])) >= RESULT_OK);
    assert(video.GetPalette().SetMode(Api::Video::Palette::MODE_YUV) >= RESULT_OK);
    assert(video.SetDecoder(Api::Video::Decoder(static_cast<Api::Video::DecoderPreset>(atoi(argv[5])))) >= RESULT_OK);
    Api::Video::RenderState render;
    render.width = 256;
    render.height = 240;
    render.bits.count = 16;
    render.bits.mask.r = 0xf800;
    render.bits.mask.g = 0x07e0;
    render.bits.mask.b = 0x001f;
    render.filter = Api::Video::RenderState::FILTER_NONE;
    assert(video.SetRenderState(render) >= RESULT_OK);
    assert(sound.SetSampleRate(32000) >= RESULT_OK);
    assert(machine.Power(true) >= RESULT_OK);
    if (machine.Is(Api::Machine::DISK))
        assert(fds.InsertDisk(0, 0) >= RESULT_OK);

    const double fps = nestopia_native_fps(machine);
    const auto warmup = unsigned(fps * replay.warmup + 0.5);
    const auto frames = unsigned(fps * replay.seconds + 0.5);
    uint64_t videoHash = 14695981039346656037ULL, pcmHash = videoHash;
    unsigned pcmFrames = 0, sideChanges = 0, redirectedFrames = 0;
    Clock::duration coreTime{}, videoTime{};
    std::vector<uint16_t> rgb(256 * 240);
    for (unsigned frame = 0; frame < warmup + frames; ++frame)
    {
        const auto *event = replay.at(frame);
        if (event && event->side >= 0)
        {
            assert(nestopia_change_side(fds, event->side));
            ++sideChanges;
        }
        short mono[641];
        Core::Sound::Output audioOutput(mono, sizeof(mono) / sizeof(mono[0]));
        Core::Video::Output videoOutput(rgb.data(), 256 * sizeof(rgb[0]));
        Core::Input::Controllers input;
        input.pad[0].buttons = replay.buttons;
        input.pad[1].buttons = 0;
        const uint16_t *directPixels = nullptr;
        const auto start = Clock::now();
#ifdef RG_NESTOPIA_RGB_OUTPUT_SUPPORTED
        assert(video.PrepareRgbFrame(true) >= RESULT_OK);
        assert(emulator.Execute(nullptr, &audioOutput, &input) >= RESULT_OK);
        const auto executed = Clock::now();
        if (!atoi(argv[6]))
            directPixels = video.GetRgbFrame();
        if (!directPixels)
            assert(video.Blit(videoOutput) >= RESULT_OK);
#else
        assert(emulator.Execute(&videoOutput, &audioOutput, &input) >= RESULT_OK);
        const auto executed = Clock::now();
#endif
        const auto converted = Clock::now();
        assert(audioOutput.length[0] <= sizeof(mono) / sizeof(mono[0]));
        if (frame >= warmup)
        {
            coreTime += executed - start;
            videoTime += converted - executed;
            pcmFrames += audioOutput.length[0];
            redirectedFrames += directPixels != nullptr;
            videoHash = benchmark_hash(videoHash, directPixels ? directPixels : rgb.data(), rgb.size() * sizeof(rgb[0]));
            pcmHash = benchmark_hash(pcmHash, mono, audioOutput.length[0] * sizeof(mono[0]));
        }
    }
    assert(machine.Power(false) >= RESULT_OK);
    const auto coreSeconds = std::chrono::duration<double>(coreTime).count();
    const auto videoSeconds = std::chrono::duration<double>(videoTime).count();
    printf("{\"native_fps\":%.6f,\"sample_rate\":%lu,\"frames\":%u,\"pcm_frames\":%u,"
           "\"disk_changes\":%u,\"direct_rgb_frames\":%u,\"core_seconds\":%.9f,\"blit_seconds\":%.9f,\"host_core_fps\":%.3f,"
           "\"rgb565_hash\":\"%016llx\",\"pcm_hash\":\"%016llx\"}\n",
           fps, (unsigned long)sound.GetSampleRate(), frames, pcmFrames, sideChanges, redirectedFrames, coreSeconds, videoSeconds,
           frames / coreSeconds, (unsigned long long)videoHash, (unsigned long long)pcmHash);
}
