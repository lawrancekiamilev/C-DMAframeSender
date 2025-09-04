# FrameSenderV1.5

FrameSender is a high-performance C++ application for streaming 16-bit grayscale video frames from HDF5 files to a DMA device. It supports both fixed-rate playback and Variable Refresh Rate (VRR) modes with precise timing control using threads and nanosecond-resolution timers.

---

## 🚀 Features

- Load all datasets (frames) from a `.h5` file into memory
- Playback modes:
  - **Fixed Rate** — Play frames sequentially at a specified frame rate
  - **VRR (Variable Refresh Rate)** — Repeat one frame with varying frame rates based on user-defined patterns
- Optional pixel value clamping
- Threaded playback with responsive signal handling
- Command-line interactive control (pause, resume, reconfigure)
- Graceful shutdown on `Ctrl+C`
- Fast writing to `/dev/xdma0_h2c_0` using raw `write()` syscall

---

## 🧾 Usage

```bash
./FrameSender <h5_path> <frame_rate> [--ply] [--vrr <pattern>]
```

### Example

```bash
./FrameSender frames.h5 1000 --vrr 100,50:500,50
```

- `--ply` — starts fixed-rate playback immediately
- `--vrr <pattern>` — enables VRR using fps,frameCount pairs (e.g., 100,50:1000,50)

---

## 🧠 Commands (Interactive Mode)

- `CLAMP` — Set max pixel value (0–65535) to clamp frame data
- `CFR` — Change frame rate
- `PLY` — Start fixed-rate playback
- `VRR` — Enter VRR mode and input new pattern
- `STP` — Stop playback
- `RST` — Reset playback index
- `HLP` — Show available commands
- `EXT` — Exit and clean up resources

---

## 🗃 HDF5 File Format

- Each dataset in the HDF5 file is treated as one 2D frame (e.g. 1024×1024).
- Datasets are read as `uint16_t` arrays.
- All datasets are loaded into `frames_buffer` for rapid access.

---

## 🧵 Threading Model

- Playback is handled in a background thread.
- Atomic flags (`stop_flag`, `reset_flag`) ensure safe control from the main thread.
- Uses `std::this_thread::sleep_until` for nanosecond-resolution timing.

---

## 📂 DMA Device

This application writes to:

```
/dev/xdma0_h2c_0
```

Make sure this device exists and is writable by the current user.

---

## 🛠 Build Instructions

### Dependencies
- GCC / Clang with C++17 support
- HDF5 C++ library (`libhdf5-dev`, `libhdf5-cpp-dev`)

### Example Build (Linux)

```bash
g++ -std=c++17 FrameSender.cpp -lhdf5 -lhdf5_cpp -o FrameSender
```

---

## 📝 License

This project is proprietary and intended for use with Chip Design Systems image rendering pipelines.
