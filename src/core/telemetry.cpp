#include "telemetry.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {
bool readFile(const std::string& path, std::string& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return !out.empty();
}
}  // namespace

bool TelemetrySampler::readNpu(Telemetry& t) {
  std::string txt;
  // Needs root: "NPU load:  Core0:  12%, Core1:   3%,"
  if (readFile("/sys/kernel/debug/rknpu/load", txt)) {
    std::vector<int> cores;
    for (size_t pos = 0; pos < txt.size(); ++pos) {
      if (txt[pos] != '%') continue;
      size_t e = pos, s = pos;
      while (s > 0 && std::isdigit(static_cast<unsigned char>(txt[s - 1]))) --s;
      if (s < e) cores.push_back(std::atoi(txt.substr(s, e - s).c_str()));
    }
    if (!cores.empty()) {
      double sum = 0;
      for (int c : cores) sum += c;
      t.npuCores = cores;
      t.npuPct = sum / cores.size();
      return true;
    }
  }
  // Fallback: devfreq node, content like "12@1000000000Hz"
  if (!devfreqScanned_) {
    devfreqScanned_ = true;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/devfreq", ec)) {
      std::string name = e.path().filename().string();
      if (name.find("npu") != std::string::npos) {
        devfreqLoadPath_ = (e.path() / "load").string();
        break;
      }
    }
  }
  if (!devfreqLoadPath_.empty() && readFile(devfreqLoadPath_, txt)) {
    t.npuPct = std::atof(txt.c_str());
    t.npuCores.clear();
    return true;
  }
  return false;
}

bool TelemetrySampler::npuAvailable() {
  Telemetry t;
  if (readNpu(t)) return true;
  std::error_code ec;
  return fs::exists("/sys/kernel/debug/rknpu", ec) || fs::exists("/dev/rknpu", ec);
}

Telemetry TelemetrySampler::sample() {
  Telemetry t;
  std::string txt;

  // CPU: delta of /proc/stat
  if (readFile("/proc/stat", txt)) {
    std::istringstream in(txt);
    std::string tag;
    unsigned long long v[8] = {0};
    in >> tag;
    for (auto& x : v) in >> x;
    unsigned long long total = 0;
    for (auto x : v) total += x;
    unsigned long long idle = v[3] + v[4];
    if (havePrev_ && total > prevTotal_) {
      double dt = static_cast<double>(total - prevTotal_);
      double di = static_cast<double>(idle - prevIdle_);
      t.cpuPct = 100.0 * (dt - di) / dt;
      t.cpuOk = true;
    }
    prevTotal_ = total;
    prevIdle_ = idle;
    havePrev_ = true;
    int n = 0;
    std::istringstream all(txt);
    std::string line;
    while (std::getline(all, line))
      if (line.compare(0, 3, "cpu") == 0 && line.size() > 3 && std::isdigit(static_cast<unsigned char>(line[3]))) ++n;
    t.cpuCount = n;
  }

  // RAM
  if (readFile("/proc/meminfo", txt)) {
    std::istringstream in(txt);
    std::string key, unit;
    long long val = 0, total = 0, avail = -1;
    while (in >> key >> val >> unit) {
      if (key == "MemTotal:") total = val;
      else if (key == "MemAvailable:") avail = val;
    }
    if (total > 0 && avail >= 0) {
      t.ramTotalMb = total / 1024.0;
      t.ramUsedMb = (total - avail) / 1024.0;
      t.ramOk = true;
    }
  }

  // Temperature: hottest thermal zone
  if (!zonesScanned_) {
    zonesScanned_ = true;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/sys/class/thermal", ec)) {
      if (e.path().filename().string().rfind("thermal_zone", 0) == 0)
        zonePaths_.push_back((e.path() / "temp").string());
    }
  }
  for (const auto& p : zonePaths_) {
    if (readFile(p, txt)) {
      double c = std::atof(txt.c_str()) / 1000.0;
      if (c > 0 && (!t.tempOk || c > t.tempC)) {
        t.tempC = c;
        t.tempOk = true;
      }
    }
  }

  t.npuOk = readNpu(t);
  return t;
}
