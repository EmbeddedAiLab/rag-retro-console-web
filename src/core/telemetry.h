// Live board telemetry: CPU, RAM, NPU load and SoC temperature.
#pragma once
#include <string>
#include <vector>

struct Telemetry {
  bool cpuOk = false;
  double cpuPct = 0;
  bool npuOk = false;
  double npuPct = 0;
  std::vector<int> npuCores;
  bool ramOk = false;
  double ramUsedMb = 0;
  double ramTotalMb = 0;
  bool tempOk = false;
  double tempC = 0;
  int cpuCount = 0;
};

class TelemetrySampler {
 public:
  Telemetry sample();
  bool npuAvailable();

 private:
  bool readNpu(Telemetry& t);

  unsigned long long prevTotal_ = 0;
  unsigned long long prevIdle_ = 0;
  bool havePrev_ = false;

  bool devfreqScanned_ = false;
  std::string devfreqLoadPath_;
  bool zonesScanned_ = false;
  std::vector<std::string> zonePaths_;
};
