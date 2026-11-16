#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::string fixtureMode() {
#ifdef _WIN32
  char mode[64]{};
  const DWORD length =
      GetEnvironmentVariableA("GROUNDZERO_TEST_PROBE_MODE", mode, sizeof(mode));
  if (length == 0 || length >= sizeof(mode))
    throw std::runtime_error("A bounded probe fixture mode is required");
#else
  const char* mode = std::getenv("GROUNDZERO_TEST_PROBE_MODE");
  if (!mode) throw std::runtime_error("Probe fixture mode is required");
#endif
  return mode;
}

std::filesystem::path movieArgument(int argc, char* argv[]) {
#ifdef _WIN32
  (void)argc;
  (void)argv;
  int count          = 0;
  const auto release = [](wchar_t** value) { LocalFree(value); };
  std::unique_ptr<wchar_t*, decltype(release)> arguments(
      CommandLineToArgvW(GetCommandLineW(), &count), release);
  if (!arguments || count < 2)
    throw std::runtime_error("Probe fixture requires an owned movie argument");
  return std::filesystem::path(arguments.get()[count - 1]);
#else
  if (argc < 2)
    throw std::runtime_error("Probe fixture requires an owned movie argument");
  return std::filesystem::path(argv[argc - 1]);
#endif
}

std::string metadata(const std::string& mode) {
  std::string result =
      R"({"streams":[{"codec_type":"video","codec_name":"mpeg4",)"
      R"("pix_fmt":"yuv420p","width":32,"height":32,"nb_read_frames":"3",)"
      R"("avg_frame_rate":"24/1"}],"format":{"duration":"0.125",)"
      R"("format_name":"mov,mp4,m4a,3gp,3g2,mj2","tags":{"major_brand":"isom"}}})";
  if (mode == "valid" || mode == "delay" || mode == "tamper") return result;
  if (mode == "malformed") return "{invalid-json";
  const std::map<std::string, std::pair<std::string, std::string>> changes = {
      {"wrong-container", {"mov,mp4,m4a,3gp,3g2,mj2", "matroska,webm"}},
      {"wrong-brand", {"\"major_brand\":\"isom\"", "\"major_brand\":\"qt  \""}},
      {"missing-codec", {"\"codec_name\":\"mpeg4\"", "\"codec_name\":\"\""}},
      {"extra-stream",
       {"\"avg_frame_rate\":\"24/1\"}]",
        "\"avg_frame_rate\":\"24/"
        "1\"},{\"codec_type\":\"audio\",\"codec_name\":\"aac\"}]"}},
      {"wrong-count", {"\"nb_read_frames\":\"3\"", "\"nb_read_frames\":\"4\""}},
      {"wrong-dimensions", {"\"width\":32", "\"width\":34"}},
      {"wrong-rate", {"24/1", "12/1"}},
      {"wrong-duration", {"0.125", "0.250"}},
      {"wrong-pixfmt", {"yuv420p", "rgb24"}},
      {"audio", {"\"codec_type\":\"video\"", "\"codec_type\":\"audio\""}}};
  const auto found = changes.find(mode);
  if (found == changes.end())
    throw std::runtime_error("Unknown probe fixture mode");
  const auto& change = found->second;
  result.replace(result.find(change.first), change.first.size(), change.second);
  return result;
}

void started(const std::filesystem::path& movie) {
  std::filesystem::path path = movie;
  path += ".probe-started";
  std::ofstream marker(path, std::ios::binary | std::ios::trunc);
  marker << "started\n";
  marker.flush();
  if (!marker) throw std::runtime_error("Cannot publish probe startup marker");
}

void tamper(const std::filesystem::path& path) {
  std::fstream movie(path, std::ios::binary | std::ios::in | std::ios::out);
  char byte = 0;
  if (!movie.get(byte))
    throw std::runtime_error("Cannot read the fixture movie");
  movie.seekp(0);
  movie.put(static_cast<char>(static_cast<unsigned char>(byte) ^ 1U));
  movie.flush();
  if (!movie) throw std::runtime_error("Cannot mutate the fixture movie");
}

void overflow() {
  std::array<char, 65536> block;
  block.fill('X');
  for (int index = 0; index < 17; ++index)
    std::cout.write(block.data(), static_cast<std::streamsize>(block.size()));
  std::cout.flush();
  std::this_thread::sleep_for(std::chrono::seconds(75));
}

int execute(const std::string& mode, const std::filesystem::path& movie) {
  started(movie);
  if (mode == "nonzero") {
    std::cerr << "Intentional probe fixture failure\n";
    return 17;
  }
  if (mode == "overflow") {
    overflow();
    return 0;
  }
  const std::string result = metadata(mode);
  if (mode == "delay") std::this_thread::sleep_for(std::chrono::seconds(75));
  if (mode == "tamper") tamper(movie);
  std::cout << result << '\n';
  std::cout.flush();
  return std::cout ? 0 : 74;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    return execute(fixtureMode(), movieArgument(argc, argv));
  } catch (const std::exception& error) {
    std::cerr << "mcp_ffprobe_fixture: " << error.what() << '\n';
    return 64;
  }
}
