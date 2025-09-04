// FrameSenderAlignedV1.5.cpp
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <hdf5.h>
#include <hdf5_hl.h>

constexpr size_t DMA_ALIGNMENT = 4096;

std::string h5_path;
double frame_rate = 1000.0;
int clamp_value = -1;
std::vector<std::pair<uint16_t*, size_t>> frames_buffer;
std::vector<std::pair<int, int>> vrr_pattern;
std::string vrr_pattern_string;
std::atomic<bool> stop_flag{false};
std::atomic<bool> reset_flag{false};
std::thread playback_thread;
int fd = -1;

enum class PlaybackMode { FixedRate, VRR };
PlaybackMode mode = PlaybackMode::FixedRate;

void* aligned_malloc(size_t size, size_t alignment) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0)
        return nullptr;
    return ptr;
}

void aligned_free(void* ptr) {
    free(ptr);
}

void signal_handler(int) {
    stop_flag.store(true);
}

void send_frame(const std::pair<uint16_t*, size_t>& frame_info) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(frame_info.first);
    size_t total_bytes = frame_info.second;
    size_t bytes_written = 0;
    while (bytes_written < total_bytes) {
        ssize_t result = write(fd, data + bytes_written, total_bytes - bytes_written);
        if (result < 0) {
            std::cerr << "DMA write error\n";
            break;
        }
        bytes_written += result;
    }
}

std::vector<std::pair<uint16_t*, size_t>> process_frames(const std::string& path) {
    std::vector<std::pair<uint16_t*, size_t>> frames;
    hid_t file_id = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) throw std::runtime_error("Failed to open HDF5 file");

    hsize_t num_objs;
    H5Gget_num_objs(file_id, &num_objs);
    for (hsize_t i = 0; i < num_objs; ++i) {
        char name[1024];
        H5Gget_objname_by_idx(file_id, i, name, 1024);
        hid_t dataset = H5Dopen(file_id, name, H5P_DEFAULT);
        hid_t space = H5Dget_space(dataset);
        hsize_t dims[2];
        H5Sget_simple_extent_dims(space, dims, nullptr);
        size_t num_elements = dims[0] * dims[1];
        size_t total_bytes = num_elements * sizeof(uint16_t);
        size_t padded_bytes = (total_bytes + DMA_ALIGNMENT - 1) & ~(DMA_ALIGNMENT - 1);
        uint16_t* aligned_frame = reinterpret_cast<uint16_t*>(aligned_malloc(padded_bytes, DMA_ALIGNMENT));
        if (!aligned_frame) throw std::runtime_error("Aligned memory allocation failed");
        std::memset(aligned_frame, 0, padded_bytes);
        H5Dread(dataset, H5T_NATIVE_USHORT, H5S_ALL, H5S_ALL, H5P_DEFAULT, aligned_frame);

        if (clamp_value >= 0) {
            for (size_t j = 0; j < num_elements; ++j)
                aligned_frame[j] = std::min(aligned_frame[j], static_cast<uint16_t>(clamp_value));
        }

        frames.emplace_back(aligned_frame, padded_bytes);
        H5Dclose(dataset);
        H5Sclose(space);
    }
    H5Fclose(file_id);
    return frames;
}

void run_playback_loop() {
    using clock = std::chrono::high_resolution_clock;
    size_t idx = 0;
    auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / frame_rate));
    auto next_time = clock::now();
    while (!stop_flag.load()) {
        if (reset_flag.load()) { idx = 0; reset_flag.store(false); }
        send_frame(frames_buffer[idx % frames_buffer.size()]);
        ++idx;
        std::cout << "[FIXED] Frame " << idx << " sent\r" << std::flush;
        next_time += period;
        std::this_thread::sleep_until(next_time);
    }
}

void run_vrr_loop() {
    size_t pattern_index = 0;
    size_t frame_index = 0;

    while (!stop_flag.load()) {
        auto [fps, count] = vrr_pattern[pattern_index];
        auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(1.0 / fps));

        for (int i = 0; i < count && !stop_flag.load(); ++i) {
            auto start = std::chrono::high_resolution_clock::now();

            // Send next frame in sequence
            send_frame(frames_buffer[frame_index % frames_buffer.size()]);
            ++frame_index;

            std::this_thread::sleep_until(start + period);
        }

        // Move to next VRR pattern entry (loop around)
        pattern_index = (pattern_index + 1) % vrr_pattern.size();
    }
}


void handle_command(std::string cmd) {
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
    if (cmd == "CLAMP") {
        std::string input;
        std::cout << "Clamp value (0–65535): ";
        std::getline(std::cin, input);
        clamp_value = std::stoi(input);
        for (auto& [ptr, _] : frames_buffer) aligned_free(ptr);
        frames_buffer.clear();
    } else if (cmd == "CFR") {
        std::string input;
        std::cout << "New frame rate: ";
        std::getline(std::cin, input);
        frame_rate = std::stod(input);
    } else if (cmd == "PLY") {
        if (playback_thread.joinable()) { stop_flag.store(true); playback_thread.join(); }
        if (frames_buffer.empty()) frames_buffer = process_frames(h5_path);
        mode = PlaybackMode::FixedRate;
        stop_flag.store(false);
        playback_thread = std::thread(run_playback_loop);
    } else if (cmd == "VRR") {
        if (playback_thread.joinable()) { stop_flag.store(true); playback_thread.join(); }
        if (frames_buffer.empty()) frames_buffer = process_frames(h5_path);
        if (frames_buffer.size() > 1)
            std::cout << "[WARN] VRR uses only the first frame\n";
        std::cout << "Enter VRR pattern (e.g. 100,100:1000,100): (FrameRate,FrameRate:FrameCount,FrameCount) ";
        std::getline(std::cin, vrr_pattern_string);
        vrr_pattern.clear();
        std::stringstream ss(vrr_pattern_string);
        std::string pair;
        while (std::getline(ss, pair, ':')) {
            int fps, cnt;
            if (sscanf(pair.c_str(), "%d,%d", &fps, &cnt) == 2) {
                vrr_pattern.emplace_back(fps, cnt);
            }
        }
        if (vrr_pattern.empty()) {
            std::cout << "Invalid VRR pattern\n"; return;
        }
        mode = PlaybackMode::VRR;
        stop_flag.store(false);
        playback_thread = std::thread(run_vrr_loop);
    } else if (cmd == "STP") {
        stop_flag.store(true);
        if (playback_thread.joinable()) playback_thread.join();
    } else if (cmd == "RST") {
        reset_flag.store(true);
    } else if (cmd == "HLP") {
        std::cout << "Commands: CLAMP, CFR, PLY, VRR, STP, RST, HLP, EXT\n";
    } else if (cmd == "EXT") {
        stop_flag.store(true);
        if (playback_thread.joinable()) playback_thread.join();
        if (fd != -1) close(fd);
        for (auto& [ptr, _] : frames_buffer) aligned_free(ptr);
        exit(0);
    } else {
        std::cout << "Unknown command. Type HLP for help.\n";
    }
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    if (argc < 3) {
        std::cerr << "Usage: <exec> <h5_path> <frame_rate>" << std::endl;
        return 1;
    }
    h5_path = argv[1];
    frame_rate = std::stod(argv[2]);

    fd = open("/dev/xdma0_h2c_0", O_WRONLY);
    if (fd == -1) {
        std::cerr << "Failed to open /dev/xdma0_h2c_0\n";
        return 1;
    }

    std::cout << "\n→ Ready: " << h5_path << ", PLY fps=" << frame_rate << std::endl;
    std::cout << "Available commands:\n"
          << "  PLY   → Start playback using a fixed frame rate for all frames.\n"
          << "          Frames will loop continuously at the specified rate (CFR).\n"
          << "\n"
          << "  VRR   → Start Variable Refresh Rate playback.\n"
          << "          You will be prompted to enter a pattern like 100,10:500,5\n"
          << "          which means: play 10 frames at 100 FPS, then 5 frames at 500 FPS,\n"
          << "          and repeat this cycle. Frames are looped continuously.\n"
          << "\n"
          << "  CFR   → Change the fixed frame rate used in PLY mode.\n"
          << "          Example: entering 250 sets the playback to 250 FPS.\n"
          << "\n"
          << "  CLAMP → Clamp all pixel values in the video to a max value.\n"
          << "          Enter a number (0–65535) to limit brightness levels,\n"
          << "          then all frames will be reloaded with clamped values.\n"
          << "\n"
          << "  RST   → Reset playback to the first frame.\n"
          << "          This applies during both PLY and VRR modes.\n"
          << "\n"
          << "  STP   → Stop the current playback thread.\n"
          << "          You can use this before switching modes or exiting.\n"
          << "\n"
          << "  HLP   → Display this command list again.\n"
          << "\n"
          << "  EXT   → Exit the program cleanly.\n"
          << "          Stops playback, frees memory, and closes device.\n";


    std::string cmd;
    while (true) {
        std::cout << "\nCMD> ";
        std::getline(std::cin, cmd);
        handle_command(cmd);
    }
    return 0;
}
