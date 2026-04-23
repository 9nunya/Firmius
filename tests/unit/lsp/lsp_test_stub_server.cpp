#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

std::string readFrame() {
  std::string header;
  std::string line;
  size_t contentLength = 0;
  bool sawContentLength = false;

  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      break;
    }
    if (line.rfind("Content-Length: ", 0) == 0) {
      contentLength = static_cast<size_t>(std::stoul(line.substr(16)));
      sawContentLength = true;
    }
  }

  if (!sawContentLength) {
    return {};
  }

  std::string body(contentLength, '\0');
  std::cin.read(body.data(), static_cast<std::streamsize>(contentLength));
  if (static_cast<size_t>(std::cin.gcount()) != contentLength) {
    return {};
  }
  return body;
}

void sendFrame(const std::string &payload) {
  std::cout << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
  std::cout.flush();
}

std::string jsonString(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char ch : value) {
    switch (ch) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out.push_back(ch);
      break;
    }
  }
  return out;
}

std::optional<std::string> extractStringField(const std::string &body,
                                              const std::string &field) {
  const std::string needle = "\"" + field + "\":\"";
  const size_t start = body.find(needle);
  if (start == std::string::npos) {
    return std::nullopt;
  }

  std::string value;
  bool escape = false;
  for (size_t i = start + needle.size(); i < body.size(); ++i) {
    const char ch = body[i];
    if (escape) {
      switch (ch) {
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      default:
        value.push_back(ch);
        break;
      }
      escape = false;
      continue;
    }
    if (ch == '\\') {
      escape = true;
      continue;
    }
    if (ch == '"') {
      return value;
    }
    value.push_back(ch);
  }
  return std::nullopt;
}

std::string extractIdField(const std::string &body) {
  const std::string needle = "\"id\":";
  const size_t start = body.find(needle);
  if (start == std::string::npos) {
    return "null";
  }

  size_t pos = start + needle.size();
  while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
    ++pos;
  }
  if (pos >= body.size()) {
    return "null";
  }

  if (body[pos] == '"') {
    ++pos;
    std::string value = "\"";
    bool escape = false;
    for (; pos < body.size(); ++pos) {
      const char ch = body[pos];
      value.push_back(ch);
      if (escape) {
        escape = false;
        continue;
      }
      if (ch == '\\') {
        escape = true;
        continue;
      }
      if (ch == '"') {
        break;
      }
    }
    return value;
  }

  size_t end = pos;
  while (end < body.size() && body[end] != ',' && body[end] != '}' &&
         std::isspace(static_cast<unsigned char>(body[end])) == 0) {
    ++end;
  }
  return body.substr(pos, end - pos);
}

std::string diagnosticsPayloadForBody(const std::string &body,
                                      const std::string &uri) {
  std::string diagnostics = "[]";
  if (body.find("triple_error_warning") != std::string::npos) {
    diagnostics =
        R"([{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"severity":1,"message":"first fail"},{"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},"severity":1,"message":"second fail"},{"range":{"start":{"line":2,"character":0},"end":{"line":2,"character":1}},"severity":2,"message":"be careful"}])";
  } else if (body.find("beta") != std::string::npos) {
    diagnostics =
        R"([{"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":4}},"severity":1,"message":"broken call"},{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"severity":2,"message":"weak type"}])";
  }

  return std::string("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\"") +
         jsonString(uri) + "\",\"diagnostics\":" + diagnostics + "}}";
}

void maybeWritePidFile(const std::optional<std::filesystem::path> &pidFile) {
  if (!pidFile.has_value()) {
    return;
  }
  std::ofstream out(*pidFile);
  out << std::this_thread::get_id();
}

} // namespace

int main(int argc, char **argv) {
  std::optional<std::filesystem::path> pidFile;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--pid-file" && i + 1 < argc) {
      pidFile = std::filesystem::path(argv[++i]);
    }
  }

  maybeWritePidFile(pidFile);
  std::cerr << "stub-started" << std::endl;

  while (true) {
    const std::string body = readFrame();
    if (body.empty()) {
      return 0;
    }

    const std::string id = extractIdField(body);
    const std::string method = extractStringField(body, "method").value_or("");
    const std::string uri = extractStringField(body, "uri").value_or("file:///dev/null");

    if (method == "initialize") {
      sendFrame(std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id +
                ",\"result\":{\"capabilities\":{\"referencesProvider\":true}}}");
    } else if (method == "shutdown") {
      sendFrame(std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id +
                ",\"result\":null}");
    } else if (method == "exit") {
      return 0;
    } else if (method == "textDocument/didOpen" || method == "textDocument/didChange") {
      sendFrame(diagnosticsPayloadForBody(body, uri));
    } else if (method == "textDocument/references") {
      sendFrame(std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id +
                ",\"result\":[{\"uri\":\"" + jsonString(uri) +
                "\",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":5}}}]}");
    } else if (id != "null") {
      sendFrame(std::string("{\"jsonrpc\":\"2.0\",\"id\":") + id +
                ",\"result\":null}");
    }
  }
}
