#include "Clipboard.hpp"

#include "utils/Base64.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace firmius::tui {

namespace {

#if !defined(_WIN32)

// popen-based binary read. Returns the raw bytes from the command's
// stdout. Empty on failure or empty output. We read in chunks to avoid
// blocking on tiny pipes.
std::vector<unsigned char> execCmdBinary(const char* cmd) {
  std::vector<unsigned char> result;
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd, "r"), pclose);
  if (!pipe) return result;
  std::array<unsigned char, 4096> buffer{};
  while (!std::feof(pipe.get())) {
    const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe.get());
    if (n > 0) {
      result.insert(result.end(), buffer.data(), buffer.data() + n);
    }
  }
  return result;
}

std::string execCmdString(const char* cmd) {
  auto bin = execCmdBinary(cmd);
  return std::string(bin.begin(), bin.end());
}

bool isWayland() {
  const char* env = std::getenv("WAYLAND_DISPLAY");
  return env != nullptr && env[0] != '\0';
}

bool isX11() {
  const char* env = std::getenv("DISPLAY");
  return env != nullptr && env[0] != '\0';
}

#endif

}  // namespace

bool Clipboard::setText(const std::string& text) {
  if (text.empty()) return false;
  // OSC 52: ESC ] 52 ; c ; <base64> BEL
  const std::string encoded = firmius::shared::base64Encode(text);
  std::string seq = "\x1b]52;c;";
  seq += encoded;
  seq += "\x07";
#if !defined(_WIN32)
  ssize_t written = ::write(STDOUT_FILENO, seq.data(), seq.size());
  return written == static_cast<ssize_t>(seq.size());
#else
  // Windows console host honors OSC 52 in recent builds via virtual
  // terminal sequences. Fire and forget.
  std::fwrite(seq.data(), 1, seq.size(), stdout);
  std::fflush(stdout);
  return true;
#endif
}

std::optional<std::string> Clipboard::getImage(std::string& mimeTypeOut) {
#if defined(__linux__)
  // Linux: try Wayland first (modern desktop), then X11.
  std::vector<unsigned char> data;
  if (isWayland()) {
    // wl-paste --list-types tells us what's on the clipboard. We probe
    // for any image/* type before paying the cost of actually reading it.
    const std::string types = execCmdString("wl-paste --list-types 2>/dev/null");
    if (types.find("image/") == std::string::npos) return std::nullopt;
    // Prefer PNG; if the clipboard has it, we get an exact match.
    if (types.find("image/png") != std::string::npos) {
      data = execCmdBinary("wl-paste --type image/png 2>/dev/null");
      mimeTypeOut = "image/png";
    } else if (types.find("image/jpeg") != std::string::npos) {
      data = execCmdBinary("wl-paste --type image/jpeg 2>/dev/null");
      mimeTypeOut = "image/jpeg";
    } else {
      return std::nullopt;
    }
  } else if (isX11()) {
    // xclip -t TARGETS -o lists available types on the X selection.
    std::string types = execCmdString(
        "xclip -selection clipboard -t TARGETS -o 2>/dev/null");
    if (types.empty()) {
      types = execCmdString("xsel -b -t TARGETS -o 2>/dev/null");
    }
    if (types.find("image/") == std::string::npos) return std::nullopt;
    if (types.find("image/png") != std::string::npos) {
      data = execCmdBinary(
          "xclip -selection clipboard -t image/png -o 2>/dev/null");
      mimeTypeOut = "image/png";
    } else if (types.find("image/jpeg") != std::string::npos) {
      data = execCmdBinary(
          "xclip -selection clipboard -t image/jpeg -o 2>/dev/null");
      mimeTypeOut = "image/jpeg";
    } else {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
  if (data.empty()) return std::nullopt;
  return firmius::shared::base64Encode(data.data(), data.size());

#elif defined(__APPLE__)
  // macOS: pbpaste with -Prefer image returns the image bytes directly.
  // We don't try to detect format — pbpaste outputs whatever the
  // clipboard holds; we tag as PNG which is overwhelmingly the common
  // case for screenshot clipboards. Wrong tag won't break the upload
  // since most providers sniff the bytes anyway.
  auto data = execCmdBinary("pbpaste -Prefer image 2>/dev/null");
  if (data.empty() || data.size() < 8) return std::nullopt;
  // Quick magic-byte sniff: PNG starts with 89 50 4E 47, JPEG with FF D8.
  if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
    mimeTypeOut = "image/png";
  } else if (data[0] == 0xFF && data[1] == 0xD8) {
    mimeTypeOut = "image/jpeg";
  } else {
    // Not a recognizable image — pbpaste fell back to text bytes.
    return std::nullopt;
  }
  return firmius::shared::base64Encode(data.data(), data.size());

#elif defined(_WIN32)
  // Windows: PowerShell can emit clipboard image as a base64 string,
  // saving us a temp-file dance. The script writes the image to a
  // memory stream, base64s it, and prints to stdout. If no image is
  // on the clipboard, Get-Clipboard returns nothing and the script
  // emits an empty line.
  const char* cmd =
      "powershell -NoProfile -NonInteractive -Command "
      "\"$img = Get-Clipboard -Format Image; "
      "if ($img) { $ms = New-Object System.IO.MemoryStream; "
      "$img.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); "
      "[Convert]::ToBase64String($ms.ToArray()); }\" 2>NUL";
  std::string out = execCmdString(cmd);
  // Strip CRLF and whitespace.
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                           out.back() == ' ')) {
    out.pop_back();
  }
  if (out.empty()) return std::nullopt;
  mimeTypeOut = "image/png";
  return out;

#else
  (void)mimeTypeOut;
  return std::nullopt;
#endif
}

}  // namespace firmius::tui
