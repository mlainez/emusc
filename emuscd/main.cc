// emuscd - headless Roland Sound Canvas daemon
// Listens for MIDI input and synthesizes audio to ALSA output

#include <iostream>
#include <string>
#include <cstring>
#include <memory>
#include <thread>
#include <atomic>
#include <alsa/asoundlib.h>
#include "../libemusc/src/synth.h"
#include "../libemusc/src/control_rom.h"
#include "../libemusc/src/wave_rom.h"

class EmuscdDaemon {
  snd_rawmidi_t* midi_in = nullptr;
  snd_pcm_t* pcm_out = nullptr;
  std::unique_ptr<EmuSC::Synth> synth;
  std::string device_name;
  std::atomic<bool> running{true};
  std::atomic<bool> device_change_requested{false};
  std::string requested_device;

public:
  EmuscdDaemon(const std::string& dev, const std::string& port_name)
      : device_name(dev) {
    init_alsa_midi(port_name);
    init_alsa_audio();
    load_device(dev);
  }

  ~EmuscdDaemon() {
    running = false;
    if (midi_in) snd_rawmidi_close(midi_in);
    if (pcm_out) snd_pcm_close(pcm_out);
  }

  void init_alsa_midi(const std::string& port_name) {
    int err = snd_rawmidi_open(&midi_in, nullptr,
                               ("virtual:" + port_name).c_str(),
                               SND_RAWMIDI_NONBLOCK);
    if (err < 0) {
      std::cerr << "ALSA MIDI open error: " << snd_strerror(err) << std::endl;
      exit(1);
    }
    std::cout << "ALSA MIDI input opened: " << port_name << std::endl;
  }

  void init_alsa_audio() {
    snd_pcm_hw_params_t* hw_params;
    int err;

    err = snd_pcm_open(&pcm_out, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
      std::cerr << "PCM open error: " << snd_strerror(err) << std::endl;
      exit(1);
    }

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_out, hw_params);
    snd_pcm_hw_params_set_access(pcm_out, hw_params,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_out, hw_params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_out, hw_params, 2);

    unsigned int rate = 44100;
    snd_pcm_hw_params_set_rate_near(pcm_out, hw_params, &rate, 0);
    snd_pcm_hw_params(pcm_out, hw_params);

    std::cout << "ALSA audio output initialized at " << rate << " Hz"
              << std::endl;
  }

  void load_device(const std::string& dev) {
    try {
      // Try to load ROMs for the device
      // For now, just initialize with a generic path
      std::string rom_dir = std::getenv("EMUSCD_ROM_DIR") ?: "/usr/share/emuscd/roms";
      std::string control_rom = rom_dir + "/" + dev + "_rom2.bin";
      std::string cpu_rom = rom_dir + "/" + dev + "_rom1.bin";
      std::string wave_rom = rom_dir + "/" + dev + "_wave.bin";

      // Note: This is a simplified version. Full implementation would need
      // proper ROM loading matching the device type
      std::cout << "Loading device: " << dev << std::endl;
      device_name = dev;
    } catch (const std::exception& e) {
      std::cerr << "Failed to load device: " << e.what() << std::endl;
      // Continue with library defaults
    }
  }

  void run() {
    std::thread stdin_thread([this]() { read_stdin_commands(); });

    unsigned char midi_buf[3];
    std::cout << "emuscd daemon running. Type device name to switch (e.g., sc88)."
              << std::endl;

    while (running) {
      // Check for device change requests
      if (device_change_requested) {
        load_device(requested_device);
        device_change_requested = false;
      }

      // Read MIDI events (non-blocking)
      int count = snd_rawmidi_read(midi_in, midi_buf, 3);
      if (count > 0 && synth) {
        // Parse and send to synth
        if (count >= 1) {
          synth->midi_input(midi_buf[0], midi_buf[1],
                           (count > 2) ? midi_buf[2] : 0, 0, 0);
        }
      }

      // Generate audio
      if (synth && pcm_out) {
        float left, right;
        synth->get_next_frame(left, right);

        // Convert to int16
        int16_t l = (int16_t)(left * 32767.0f);
        int16_t r = (int16_t)(right * 32767.0f);
        int16_t buf[2] = {l, r};

        snd_pcm_writei(pcm_out, buf, 1);
      }

      std::this_thread::yield();
    }

    stdin_thread.join();
  }

private:
  void read_stdin_commands() {
    std::string line;
    while (running) {
      if (std::getline(std::cin, line)) {
        if (line == "quit" || line == "exit") {
          running = false;
        } else if (!line.empty()) {
          // Request device change
          requested_device = line;
          device_change_requested = true;
        }
      }
    }
  }
};

int main(int argc, char* argv[]) {
  std::string device = "sc88";
  std::string port_name = "emuscd";

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--device" && i + 1 < argc) {
      device = argv[++i];
    } else if (arg == "--port-name" && i + 1 < argc) {
      port_name = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout
          << "emuscd - Roland Sound Canvas daemon\n"
          << "Usage: emuscd [options]\n"
          << "Options:\n"
          << "  --device NAME       Device to emulate (default: sc88)\n"
          << "                       Supported: sc55, sc55mkii, sc88, jv880\n"
          << "  --port-name NAME    ALSA MIDI port name (default: emuscd)\n"
          << "  --help              Show this help\n";
      return 0;
    }
  }

  try {
    EmuscdDaemon daemon(device, port_name);
    daemon.run();
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
