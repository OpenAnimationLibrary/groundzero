#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
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

struct Invocation {
  std::vector<std::string> options;
  std::filesystem::path movie;
};

#ifdef _WIN32
std::string asciiOption(const wchar_t* value) {
  std::string option;
  for (; *value; ++value) {
    if (*value > 127)
      throw std::runtime_error("Probe option fields must be ASCII");
    option.push_back(static_cast<char>(*value));
  }
  return option;
}
#endif

Invocation invocation(int argc, char* argv[]) {
  Invocation result;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  int count          = 0;
  const auto release = [](wchar_t** value) { LocalFree(value); };
  std::unique_ptr<wchar_t*, decltype(release)> arguments(
      CommandLineToArgvW(GetCommandLineW(), &count), release);
  if (!arguments || count < 2)
    throw std::runtime_error("Probe fixture requires an owned movie argument");
  for (int index = 1; index < count - 1; ++index)
    result.options.push_back(asciiOption(arguments.get()[index]));
  result.movie = std::filesystem::path(arguments.get()[count - 1]);
#else
  if (argc < 2)
    throw std::runtime_error("Probe fixture requires an owned movie argument");
  result.options.assign(argv + 1, argv + argc - 1);
  result.movie = std::filesystem::path(argv[argc - 1]);
#endif
  return result;
}

std::string optionValue(const std::vector<std::string>& options,
                        const std::string& key) {
  if (std::count(options.begin(), options.end(), key) != 1)
    throw std::runtime_error("Probe fixture requires one " + key + " option");
  const auto found = std::find(options.begin(), options.end(), key);
  if (found + 1 == options.end())
    throw std::runtime_error("Probe fixture option has no value: " + key);
  return *(found + 1);
}

bool hasField(const std::string& query, const std::string& section,
              const std::string& field) {
  std::istringstream sections(query);
  std::string entry;
  const std::string prefix = section + "=";
  while (std::getline(sections, entry, ':')) {
    if (entry.compare(0, prefix.size(), prefix) != 0) continue;
    std::istringstream fields(entry.substr(prefix.size()));
    std::string name;
    while (std::getline(fields, name, ','))
      if (name == field) return true;
  }
  return false;
}

void requireField(const std::string& query, const std::string& section,
                  const std::string& field) {
  if (!hasField(query, section, field))
    throw std::runtime_error("Probe command omits " + section + "." + field);
}

void validateInvocation(const Invocation& call) {
  const auto& options = call.options;
  if (std::find(options.begin(), options.end(), "-count_frames") ==
      options.end())
    throw std::runtime_error("Probe command must count decoded frames");
  for (const auto& option : options)
    if (option.compare(0, 15, "-select_streams") == 0)
      throw std::runtime_error("Probe command must inspect every stream");
  if (optionValue(options, "-of") != "json")
    throw std::runtime_error("Probe command must request JSON output");
  const std::string query = optionValue(options, "-show_entries");
  for (const char* field : {"codec_type", "codec_name", "pix_fmt", "width",
                            "height", "nb_read_frames", "avg_frame_rate"})
    requireField(query, "stream", field);
  requireField(query, "format", "format_name");
  requireField(query, "format", "duration");
  requireField(query, "format_tags", "major_brand");
}

bool isEncoder(const Invocation& call) {
  return call.options.size() >= 2 && call.options[0] == "-f" &&
         call.options[1] == "rawvideo";
}

void validateEncoder(const Invocation& call) {
  if (optionValue(call.options, "-i") != "-")
    throw std::runtime_error("Encoder fixture requires native stdin input");
  const auto input = std::find(call.options.begin(), call.options.end(), "-i");
  const std::vector<std::string> prefix(call.options.begin(), input);
  if (optionValue(prefix, "-video_size") != "32x32")
    throw std::runtime_error("Encoder fixture requires 32x32 frames");
  const std::string format = optionValue(prefix, "-pix_fmt");
  const std::array<std::string, 4> formats{"rgba", "bgra", "argb", "abgr"};
  if (std::find(formats.begin(), formats.end(), format) == formats.end())
    throw std::runtime_error("Encoder fixture requires native 32-bit pixels");
}

std::streamsize drainEncoderInput() {
#ifdef _WIN32
  if (_setmode(_fileno(stdin), _O_BINARY) == -1)
    throw std::runtime_error("Cannot read encoder stdin as binary data");
#endif
  std::array<char, 4096> buffer;
  std::streamsize bytes = 0;
  while (std::cin.read(buffer.data(),
                       static_cast<std::streamsize>(buffer.size())) ||
         std::cin.gcount()) {
    bytes += std::cin.gcount();
    if (bytes > 3 * 32 * 32 * 4)
      throw std::runtime_error("Encoder fixture received too many frame bytes");
  }
  if (!std::cin.eof() || bytes != 3 * 32 * 32 * 4)
    throw std::runtime_error(
        "Encoder fixture did not receive exactly three frames");
  return bytes;
}

void publishEncoderMarker(const std::filesystem::path& movie,
                          std::streamsize bytes, unsigned long pid) {
  std::filesystem::path path = movie;
  path += ".encoder-eof.json";
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  std::ofstream marker(temporary, std::ios::binary | std::ios::trunc);
  marker << "{\"pid\":" << pid << ",\"input_bytes\":" << bytes << "}\n";
  marker.close();
  if (!marker) throw std::runtime_error("Cannot publish encoder EOF marker");
  std::filesystem::rename(temporary, path);
}

int stallEncoder(const Invocation& call) {
  validateEncoder(call);
  const std::streamsize bytes = drainEncoderInput();
  std::ofstream movie(call.movie, std::ios::binary | std::ios::trunc);
  movie << "Incomplete fixed encoder fixture output\n";
  movie.close();
  if (!movie) throw std::runtime_error("Cannot write partial encoder output");
#ifdef _WIN32
  const auto pid = GetCurrentProcessId();
#else
  const auto pid = getpid();
#endif
  publishEncoderMarker(call.movie, bytes, static_cast<unsigned long>(pid));
  std::this_thread::sleep_for(std::chrono::seconds(75));
  return 0;
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

// Answer the runtime media probe and the writer registration query as a
// media tool whose mp4 muxer has an h264 encoder; -1 for other calls.
int answerQuery(int argc, char* argv[]) {
  const std::vector<std::string> arguments(argv + 1, argv + argc);
  using Arguments = std::vector<std::string>;
  if (arguments == Arguments{"-formats"})
    std::cout << " DE mp4             MP4 (MPEG-4 Part 14)\n";
  else if (arguments == Arguments{"-hide_banner", "-version"})
    std::cout << "ffmpeg version fixture\n"
                 "libavcodec     62. 28.102 / 62. 28.102\n";
  else if (arguments == Arguments{"-hide_banner", "-h", "muxer=mp4"})
    std::cout << "Muxer mp4 [MP4 (MPEG-4 Part 14)]:\n"
                 "    Default video codec: h264.\n";
  else if (arguments == Arguments{"-hide_banner", "-encoders"})
    std::cout << "Encoders:\n ------\n"
                 " V....D fixture264           Fixture encoder (codec h264)\n";
  else
    return -1;
  std::cout.flush();
  return std::cout ? 0 : 74;
}

int main(int argc, char* argv[]) {
  try {
    const int answered = answerQuery(argc, argv);
    if (answered >= 0) return answered;
    const Invocation call = invocation(argc, argv);
    if (isEncoder(call)) return stallEncoder(call);
    validateInvocation(call);
    return execute(fixtureMode(), call.movie);
  } catch (const std::exception& error) {
    std::cerr << "mcp_ffprobe_fixture: " << error.what() << '\n';
    return 64;
  }
}
