// SPDX-License-Identifier: MIT
// cpp-cpu-stat — Linux per-core CPU utilization monitor in C++23
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o cpu-stat
// Usage: ./cpu-stat [--watch SECONDS] [--json] [--no-color] [--selftest] [--help]

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace cpustat {

// ── ANSI palette (Catppuccin-ish) ──────────────────────────────────────────
constexpr const char* kRed    = "\x1b[31m";
constexpr const char* kGreen  = "\x1b[32m";
constexpr const char* kYellow = "\x1b[33m";
constexpr const char* kBlue   = "\x1b[34m";
constexpr const char* kMagenta = "\x1b[35m";
constexpr const char* kCyan   = "\x1b[36m";
constexpr const char* kBold   = "\x1b[1m";
constexpr const char* kDim    = "\x1b[2m";
constexpr const char* kReset  = "\x1b[0m";

// ── Types ─────────────────────────────────────────────────────────────────
struct CpuTimes {
  std::string name;       // "cpu", "cpu0", "cpu1", ...
  unsigned long long user = 0;
  unsigned long long nice = 0;
  unsigned long long system = 0;
  unsigned long long idle = 0;
  unsigned long long iowait = 0;
  unsigned long long irq = 0;
  unsigned long long softirq = 0;
  unsigned long long steal = 0;
};

struct CpuPct {
  std::string name;
  double user = 0.0;
  double nice = 0.0;
  double system = 0.0;
  double idle = 0.0;
  double iowait = 0.0;
  double irq = 0.0;
  double softirq = 0.0;
  double steal = 0.0;
};

struct SystemContext {
  double load1 = 0.0, load5 = 0.0, load15 = 0.0;
  double uptime = 0.0;
  int running = 0, total = 0;
  int last_pid = 0;
};

struct Sample {
  std::vector<CpuTimes> cpus;
  SystemContext ctx;
};

// ── /proc/stat parsing ────────────────────────────────────────────────────
// cpu  user nice system idle iowait irq softirq steal guest guest_nice
// cpu0 ...
[[maybe_unused]] static std::optional<CpuTimes> parse_cpu_line(const std::string& line) {
  if (!line.starts_with("cpu")) return std::nullopt;
  if (line.size() < 5) return std::nullopt;

  // Extract name: "cpu", "cpu0", "cpu12"
  auto rest = std::string_view(line);
  auto space = rest.find(' ');
  if (space == std::string_view::npos) return std::nullopt;
  std::string name(rest.substr(0, space));
  rest.remove_prefix(space + 1);

  // Skip leading spaces
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
    rest.remove_prefix(1);

  CpuTimes ct;
  ct.name = name;

  unsigned long long vals[9] = {0};
  int idx = 0;
  while (!rest.empty() && idx < 9) {
    auto start = rest.data();
    auto end = rest.data() + rest.size();
    auto [ptr, ec] = std::from_chars(start, end, vals[idx]);
    if (ec != std::errc{}) break;
    ++idx;
    auto consumed = ptr - start;
    rest.remove_prefix(consumed);
    while (!rest.empty() && rest.front() == ' ')
      rest.remove_prefix(1);
  }

  if (idx < 4) return std::nullopt;  // need at least user+nice+system+idle

  ct.user    = vals[0];
  ct.nice    = vals[1];
  ct.system  = vals[2];
  ct.idle    = vals[3];
  ct.iowait  = (idx > 4) ? vals[4] : 0;
  ct.irq     = (idx > 5) ? vals[5] : 0;
  ct.softirq = (idx > 6) ? vals[6] : 0;
  ct.steal   = (idx > 7) ? vals[7] : 0;

  return ct;
}

std::vector<CpuTimes> read_cpu_times() {
  std::ifstream f("/proc/stat");
  if (!f.is_open()) return {};

  std::vector<CpuTimes> result;
  std::string line;
  while (std::getline(f, line)) {
    if (!line.starts_with("cpu")) break;
    if (auto ct = parse_cpu_line(line); ct)
      result.push_back(std::move(*ct));
  }
  return result;
}

SystemContext read_system_context() {
  SystemContext ctx;

  // /proc/loadavg
  std::ifstream la("/proc/loadavg");
  if (la.is_open()) {
    std::string a, b, c, d, e;
    la >> a >> b >> c >> d >> e;
    if (!a.empty()) ctx.load1 = std::atof(a.c_str());
    if (!b.empty()) ctx.load5 = std::atof(b.c_str());
    if (!c.empty()) ctx.load15 = std::atof(c.c_str());
    if (!d.empty()) {
      auto slash = d.find('/');
      if (slash != std::string::npos) {
        ctx.running = std::atoi(d.substr(0, slash).c_str());
        ctx.total = std::atoi(d.substr(slash + 1).c_str());
      }
    }
    if (!e.empty()) ctx.last_pid = std::atoi(e.c_str());
  }

  // /proc/uptime
  std::ifstream up("/proc/uptime");
  if (up.is_open()) {
    std::string a, b;
    up >> a >> b;
    if (!a.empty()) ctx.uptime = std::atof(a.c_str());
  }

  return ctx;
}

Sample read_sample() {
  return {read_cpu_times(), read_system_context()};
}

// ── Delta computation ─────────────────────────────────────────────────────
CpuPct compute_pct(const CpuTimes& prev, const CpuTimes& cur) {
  auto delta = [](unsigned long long a, unsigned long long b) -> unsigned long long {
    return (b > a) ? (b - a) : 0;
  };

  unsigned long long d_user    = delta(prev.user, cur.user);
  unsigned long long d_nice    = delta(prev.nice, cur.nice);
  unsigned long long d_system  = delta(prev.system, cur.system);
  unsigned long long d_idle    = delta(prev.idle, cur.idle);
  unsigned long long d_iowait  = delta(prev.iowait, cur.iowait);
  unsigned long long d_irq     = delta(prev.irq, cur.irq);
  unsigned long long d_softirq = delta(prev.softirq, cur.softirq);
  unsigned long long d_steal   = delta(prev.steal, cur.steal);

  unsigned long long total = d_user + d_nice + d_system + d_idle + d_iowait
                           + d_irq + d_softirq + d_steal;

  CpuPct pct;
  pct.name = cur.name;
  if (total == 0) return pct;

  auto p = [total](unsigned long long v) -> double {
    return 100.0 * static_cast<double>(v) / static_cast<double>(total);
  };

  pct.user    = p(d_user);
  pct.nice    = p(d_nice);
  pct.system  = p(d_system);
  pct.idle    = p(d_idle);
  pct.iowait  = p(d_iowait);
  pct.irq     = p(d_irq);
  pct.softirq = p(d_softirq);
  pct.steal   = p(d_steal);

  return pct;
}

std::vector<CpuPct> compute_all_pct(const std::vector<CpuTimes>& prev,
                                     const std::vector<CpuTimes>& cur) {
  std::vector<CpuPct> result;
  size_t n = std::min(prev.size(), cur.size());
  for (size_t i = 0; i < n; ++i) {
    if (prev[i].name == cur[i].name)
      result.push_back(compute_pct(prev[i], cur[i]));
  }
  return result;
}

// ── Formatting helpers ────────────────────────────────────────────────────
std::string fmt_duration(double seconds) {
  long long s = static_cast<long long>(seconds);
  int d = static_cast<int>(s / 86400);
  int h = static_cast<int>((s % 86400) / 3600);
  int m = static_cast<int>((s % 3600) / 60);
  int sec = static_cast<int>(s % 60);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%dd %02d:%02d:%02d", d, h, m, sec);
  return buf;
}

[[maybe_unused]] static std::string cpu_bar(double pct, int width = 16) {
  int filled = std::clamp(static_cast<int>(std::round(pct / 100.0 * width)), 0, width);
  std::string bar;
  bar.reserve(width + 2);
  bar += '[';
  for (int i = 0; i < filled; ++i) bar += '|';
  for (int i = filled; i < width; ++i) bar += '.';
  bar += ']';
  return bar;
}

// ── Rendering ─────────────────────────────────────────────────────────────
void print_banner(bool color) {
  const char* c = color ? kCyan : "";
  const char* b = color ? kBold : "";
  const char* r = color ? kReset : "";
  std::println(std::cout, "{}{}cpu-stat{} — Linux per-core CPU utilization monitor (C++23)", c, b, r);
  std::println(std::cout);
}

void render_table(const std::vector<CpuPct>& pcts, bool color) {
  const char* r = color ? kReset : "";   // reset for color-wrapped cells
  // Header
  std::println(std::cout, "  {:<8} {:>6} {:>6} {:>6} {:>6} {:>6} {:>5} {:>7} {:>6}",
               "CORE", "user", "nice", "sys", "idle", "iowait", "irq", "softirq", "steal");

  for (const auto& p : pcts) {
    auto pct_color = [&p, color](double v) -> const char* {
      if (!color) return "";
      if (p.name == "cpu") return cpustat::kBold;
      if (v >= 50.0) return cpustat::kRed;
      if (v >= 25.0) return cpustat::kYellow;
      return cpustat::kGreen;
    };

    std::string core_display = p.name;
    if (p.name == "cpu") core_display = "total";

    char buf[512];
    int n = std::snprintf(buf, sizeof(buf),
      "  %-8s %s%5.1f%s  %5.1f  %s%5.1f%s  %s%5.1f%s  %5.1f  %5.1f  %5.1f  %5.1f",
      core_display.c_str(),
      pct_color(p.user), p.user, r,
      p.nice,
      pct_color(p.system), p.system, r,
      pct_color(p.idle), p.idle, r,
      p.iowait, p.irq, p.softirq, p.steal);
    if (n > 0) std::cout << buf << '\n';
  }
}

void render_context(const SystemContext& ctx, bool color) {
  const char* d = color ? kDim : "";
  const char* r = color ? kReset : "";
  std::println(std::cout, "  {}load{:>8} {:5.2f} {:5.2f} {:5.2f}   {}running{:>5} {}/{}  pid{} {}",
               d, "", ctx.load1, ctx.load5, ctx.load15,
               d, "", ctx.running, ctx.total, ctx.last_pid, r);
  std::println(std::cout, "  {}uptime{:>6}{}{}",
               d, "", fmt_duration(ctx.uptime), r);
}

void render_json(const std::vector<CpuPct>& pcts, const SystemContext& ctx) {
  std::println(std::cout, "{{");
  std::println(std::cout, "  \"cpus\": [");
  for (size_t i = 0; i < pcts.size(); ++i) {
    const auto& p = pcts[i];
    std::println(std::cout, "    {{\"core\": \"{}\", \"user\": {:.1f}, \"nice\": {:.1f}, \"system\": {:.1f}, "
                 "\"idle\": {:.1f}, \"iowait\": {:.1f}, \"irq\": {:.1f}, \"softirq\": {:.1f}, \"steal\": {:.1f}}}{}",
                 p.name, p.user, p.nice, p.system, p.idle, p.iowait, p.irq, p.softirq, p.steal,
                 (i + 1 < pcts.size()) ? "," : "");
  }
  std::println(std::cout, "  ],");
  std::println(std::cout, "  \"loadavg\": {{ \"1m\": {:.2f}, \"5m\": {:.2f}, \"15m\": {:.2f} }},", ctx.load1, ctx.load5, ctx.load15);
  std::println(std::cout, "  \"running\": {}, \"total\": {}, \"uptime\": {:.2f}", ctx.running, ctx.total, ctx.uptime);
  std::println(std::cout, "}}");
}

// ── Selftest ──────────────────────────────────────────────────────────────
int run_selftest() {
  int passed = 0, total = 0;

  // Test 1: parse typical cpu line
  total++;
  {
    auto r = parse_cpu_line("cpu  123456 7890 45678 99999999 1234 567 89 12");
    if (r && r->name == "cpu" && r->user == 123456 && r->nice == 7890
        && r->system == 45678 && r->idle == 99999999 && r->iowait == 1234
        && r->irq == 567 && r->softirq == 89 && r->steal == 12)
      passed++;
  }

  // Test 2: parse cpu0 line
  total++;
  {
    auto r = parse_cpu_line("cpu0 100 200 300 4000");
    if (r && r->name == "cpu0" && r->user == 100 && r->nice == 200
        && r->system == 300 && r->idle == 4000 && r->iowait == 0)
      passed++;
  }

  // Test 3: parse cpu12 (multi-digit)
  total++;
  {
    auto r = parse_cpu_line("cpu12 111 222 333 4444 55 66 77 88");
    if (r && r->name == "cpu12" && r->user == 111)
      passed++;
  }

  // Test 4: reject non-cpu line
  total++;
  {
    auto r = parse_cpu_line("intr 12345 0 0");
    if (!r) passed++;
  }

  // Test 5: reject empty line
  total++;
  {
    auto r = parse_cpu_line("");
    if (!r) passed++;
  }

  // Test 6: compute_pct with known values
  total++;
  {
    CpuTimes prev{"cpu", 100, 50, 200, 1000, 20, 5, 10, 2};
    CpuTimes cur{"cpu",  200, 80, 350, 2000, 40, 10, 20, 5};
    auto pct = compute_pct(prev, cur);
    // Total delta: 100+30+150+1000+20+5+10+3 = 1318
    // user% = 100/1318*100 = 7.59
    // idle% = 1000/1318*100 = 75.87
    bool ok = std::abs(pct.user - 7.59) < 0.1
           && std::abs(pct.idle - 75.87) < 0.1
           && std::abs(pct.system - 11.38) < 0.1;
    if (ok) passed++;
  }

  // Test 7: compute_pct with no delta
  total++;
  {
    CpuTimes prev{"cpu0", 100, 50, 200, 1000};
    CpuTimes cur{"cpu0",  100, 50, 200, 1000};
    auto pct = compute_pct(prev, cur);
    if (pct.idle == 0.0) passed++;
  }

  // Test 8: read /proc/stat returns data
  total++;
  {
    auto times = read_cpu_times();
    if (!times.empty() && times[0].name == "cpu") passed++;
  }

  // Test 9: read_system_context returns loadavg
  total++;
  {
    auto ctx = read_system_context();
    if (ctx.uptime > 0.0 && ctx.load1 >= 0.0) passed++;
  }

  // Test 10: compute_all_pct handles multiple cores
  total++;
  {
    std::vector<CpuTimes> prev = {
      {"cpu", 1000, 500, 2000, 10000, 100, 50, 100, 20},
      {"cpu0", 500, 250, 1000, 5000, 50, 25, 50, 10},
    };
    std::vector<CpuTimes> cur = {
      {"cpu", 2000, 800, 3500, 20000, 200, 100, 200, 40},
      {"cpu0", 1000, 400, 1750, 10000, 100, 50, 100, 20},
    };
    auto pcts = compute_all_pct(prev, cur);
    if (pcts.size() == 2) passed++;
  }

  // Test 11: non-cpu /proc/stat lines
  total++;
  {
    auto r = parse_cpu_line("btime 1234567890");
    if (!r) passed++;
  }

  // Test 12: fmt_duration formatting
  total++;
  {
    auto s = fmt_duration(90061.0);  // 1d 01:01:01
    if (s.find("1d") != std::string::npos) passed++;
  }

  // Test 13: parse line with extra whitespace
  total++;
  {
    auto r = parse_cpu_line("cpu   100   200   300   4000");
    if (r && r->user == 100 && r->system == 300) passed++;
  }

  // Test 14: computation with name mismatch
  total++;
  {
    CpuTimes prev{"cpu0", 100, 0, 0, 1000};
    CpuTimes cur{"cpu1",  200, 0, 0, 2000};
    auto pct = compute_pct(prev, cur);
    // Names differ but computation still works (just delta math)
    if (pct.name == "cpu1") passed++;
  }

  // Test 15: verify single core with only 4 fields works
  total++;
  {
    auto r = parse_cpu_line("cpu7 100 200 300 4000");
    if (r && r->name == "cpu7" && r->iowait == 0 && r->irq == 0) passed++;
  }

  std::println(std::cout, "{}/{} selftests passed", passed, total);
  return (passed == total) ? 0 : 2;
}

// ── Watch mode helpers ────────────────────────────────────────────────────
volatile std::sig_atomic_t g_running = 1;
extern "C" void handle_signal(int) { g_running = 0; }

void run_watch(int interval, bool color) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::println(std::cout, "{}refreshing every {}s — Ctrl+C to stop{}",
               (color ? kDim : ""), interval, (color ? kReset : ""));
  std::println(std::cout);

  auto prev = read_sample();
  std::this_thread::sleep_for(std::chrono::seconds(interval));

  while (g_running) {
    auto cur = read_sample();
    auto pcts = compute_all_pct(prev.cpus, cur.cpus);

    std::cout << "\x1b[2J\x1b[H";  // clear screen
    print_banner(color);
    render_table(pcts, color);
    std::println(std::cout);
    render_context(cur.ctx, color);
    std::cout << std::flush;

    prev = cur;
    for (int i = 0; i < interval && g_running; ++i)
      std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::println(std::cout, "\n{}cpu-stat stopped{}", (color ? kDim : ""), (color ? kReset : ""));
}

} // namespace cpustat

// ── Main ──────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  bool color = true;
  bool json = false;
  bool selftest = false;
  int watch_interval = 0;
  bool help = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--no-color" || arg == "--no-colour") color = false;
    else if (arg == "--json") json = true;
    else if (arg == "--selftest") selftest = true;
    else if (arg == "--help" || arg == "-h") help = true;
    else if (arg == "--watch" && i + 1 < argc) watch_interval = std::atoi(argv[++i]);
    else if (arg.starts_with("--watch=")) watch_interval = std::atoi(arg.data() + 8);
    else {
      std::println(std::cerr, "Unknown option: {}", arg);
      return 2;
    }
  }

  if (help) {
    std::println(std::cout, "Usage: cpu-stat [options]");
    std::println(std::cout);
    std::println(std::cout, "  --help, -h      Show this help");
    std::println(std::cout, "  --watch SECONDS  Refresh every SECONDS (Ctrl+C to stop)");
    std::println(std::cout, "  --json          Output as JSON");
    std::println(std::cout, "  --no-color      Disable ANSI colors");
    std::println(std::cout, "  --selftest      Run self-tests");
    std::println(std::cout);
    std::println(std::cout, "Reads /proc/stat for per-core CPU utilization breakdown.");
    return 0;
  }

  if (selftest) {
    return cpustat::run_selftest();
  }

  auto sample = cpustat::read_sample();
  // Need two samples for percentages; sleep briefly for delta
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto sample2 = cpustat::read_sample();
  auto pcts = cpustat::compute_all_pct(sample.cpus, sample2.cpus);

  if (json) {
    cpustat::render_json(pcts, sample2.ctx);
    return 0;
  }

  if (watch_interval > 0) {
    cpustat::run_watch(watch_interval, color);
    return 0;
  }

  cpustat::print_banner(color);
  cpustat::render_table(pcts, color);
  std::println(std::cout);
  cpustat::render_context(sample2.ctx, color);

  return 0;
}
