#pragma once
#include "core.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#ifndef FK_COMMIT
#define FK_COMMIT "unknown"
#endif

namespace fk {

inline Tab make_tab(int W) {
  Tab T;
  std::memset(&T, 0, sizeof(T));
  T.W = W;
  T.f[0][0] = 1;
  for (int k = 1; k <= MAXW; ++k) {
    for (int h = 0; h <= MAXW + 5; ++h) {
      uint64_t v = T.f[k - 1][h];
      if (h + 1 <= MAXW + 5) v += T.f[k - 1][h + 1];
      if (h > 0) v += T.f[k - 1][h - 1];
      T.f[k][h] = v;
    }
  }
  T.r32 = 0;
  for (int k = 0; k <= MAXW; ++k) {
    bool fits = true;
    for (int h = 0; h <= MAXW + 5; ++h) {
      if (T.f[k][h] > 0xFFFFFFFFull) fits = false;
      T.f32[k][h] = (uint32_t)T.f[k][h];
    }
    if (fits) T.r32 = k;
    else break;
  }
  T.nmain = T.f[W][1];
  T.nblk = T.f[W - 1][1];
  T.total = T.nmain + T.nblk;
  return T;
}

inline uint64_t init_index(const Tab& T) { return rank_state(T, put(0, 0, 2), 0); }

inline uint64_t answer_index(const Tab& T) {
  return rank_state(T, put(0, T.W - 1, 2), T.W - 1);
}

inline bool is_prime(uint32_t x) {
  if (x < 2) return false;
  for (uint32_t d = 2; (uint64_t)d * d <= x; ++d)
    if (x % d == 0) return false;
  return true;
}

inline uint32_t prime_limit(int bits) { return bits == 16 ? 65535u : (1u << 27) - 1; }

inline std::vector<uint32_t> prime_list(int count, int bits) {
  std::vector<uint32_t> ps;
  uint32_t x = prime_limit(bits);
  if (x % 2 == 0) --x;
  for (; (int)ps.size() < count; x -= 2)
    if (is_prime(x)) ps.push_back(x);
  return ps;
}

inline uint64_t powmod(uint64_t a, uint64_t e, uint64_t m) {
  uint64_t r = 1 % m;
  a %= m;
  while (e) {
    if (e & 1) r = r * a % m;
    a = a * a % m;
    e >>= 1;
  }
  return r;
}

struct Big {
  std::vector<uint32_t> d;
  void mul_add(uint32_t m, uint32_t a) {
    uint64_t carry = a;
    for (auto& x : d) {
      uint64_t v = (uint64_t)x * m + carry;
      x = (uint32_t)v;
      carry = v >> 32;
    }
    if (carry) d.push_back((uint32_t)carry);
  }
  uint32_t mod(uint32_t m) const {
    uint64_t r = 0;
    for (size_t i = d.size(); i-- > 0;) r = ((r << 32) | d[i]) % m;
    return (uint32_t)r;
  }
  std::string dec() const {
    std::vector<uint32_t> t = d;
    std::vector<uint32_t> parts;
    while (!t.empty()) {
      uint64_t r = 0;
      for (size_t i = t.size(); i-- > 0;) {
        uint64_t v = (r << 32) | t[i];
        t[i] = (uint32_t)(v / 1000000000u);
        r = v % 1000000000u;
      }
      parts.push_back((uint32_t)r);
      while (!t.empty() && t.back() == 0) t.pop_back();
    }
    if (parts.empty()) return "0";
    std::string s = std::to_string(parts.back());
    char buf[16];
    for (size_t i = parts.size() - 1; i-- > 0;) {
      std::snprintf(buf, sizeof(buf), "%09u", parts[i]);
      s += buf;
    }
    return s;
  }
};

inline Big crt(const std::vector<uint32_t>& ps, const std::vector<uint32_t>& rs) {
  size_t k = ps.size();
  std::vector<uint64_t> dg(k);
  for (size_t i = 0; i < k; ++i) {
    uint64_t t = rs[i] % ps[i];
    for (size_t j = 0; j < i; ++j) {
      uint64_t inv = powmod(ps[j] % ps[i], ps[i] - 2, ps[i]);
      t = (t + ps[i] - dg[j] % ps[i]) % ps[i] * inv % ps[i];
    }
    dg[i] = t;
  }
  Big x;
  for (size_t i = k; i-- > 0;) x.mul_add(ps[i], (uint32_t)dg[i]);
  while (!x.d.empty() && x.d.back() == 0) x.d.pop_back();
  return x;
}

inline int default_prime_count(int W, int bits) {
  double need = 0.81 * W * W + 32;
  double per = std::log2((double)prime_limit(bits)) - 0.01;
  return (int)std::ceil(need / per) + 1;
}

inline double now_sec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct PassOut {
  std::vector<uint32_t> res;
  std::vector<std::vector<uint32_t>> rect;
  std::vector<std::vector<std::array<uint64_t, 2>>> fp;
};

struct Runner {
  virtual ~Runner() {}
  virtual const char* name() const = 0;
  int variant = 1;
  int tpb = 512;
  int bps = 16;
  int gpus = 0;
  std::vector<std::string> info;
  virtual PassOut pass(const Tab& T, const std::vector<uint32_t>& ps, int bits) = 0;
  virtual double bytes_per_pass(const Tab& T, int lanes, int bits) const {
    return (double)T.total * (bits / 8) * lanes;
  }
};

struct Sha256 {
  uint32_t h[8];
  uint64_t len = 0;
  unsigned char buf[64];
  size_t n = 0;
  static uint32_t rotr(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
  Sha256() {
    const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h, init, sizeof(h));
  }
  void block(const unsigned char* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
             p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  void update(const unsigned char* p, size_t m) {
    len += m;
    while (m > 0) {
      size_t t = std::min(m, (size_t)64 - n);
      std::memcpy(buf + n, p, t);
      n += t;
      p += t;
      m -= t;
      if (n == 64) {
        block(buf);
        n = 0;
      }
    }
  }
  std::string hex() {
    uint64_t bits = len * 8;
    unsigned char pad = 0x80;
    update(&pad, 1);
    unsigned char z = 0;
    while (n != 56) update(&z, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    update(lb, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) std::snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 64);
  }
};

inline std::string sha256_file(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return "unavailable";
  Sha256 s;
  unsigned char b[1 << 16];
  size_t m;
  while ((m = std::fread(b, 1, sizeof(b), f)) > 0) s.update(b, m);
  std::fclose(f);
  return s.hex();
}

inline std::string utc_now() {
  std::time_t t = std::time(nullptr);
  std::tm g;
  gmtime_r(&t, &g);
  char b[32];
  std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &g);
  return b;
}

struct Results {
  std::map<uint32_t, uint32_t> res;
  std::map<int, std::map<uint32_t, uint32_t>> rect;
  int cols = 0;
  bool add(const std::string& line, std::string& err) {
    std::istringstream ss(line);
    std::string w;
    ss >> w;
    if (w == "residue") {
      uint64_t p, r;
      if (!(ss >> p >> r)) return true;
      auto it = res.find((uint32_t)p);
      if (it != res.end() && it->second != r) {
        err = "conflicting residue for " + std::to_string(p);
        return false;
      }
      res[(uint32_t)p] = (uint32_t)r;
    } else if (w == "rect") {
      uint64_t p, r;
      int rows, c;
      if (!(ss >> p >> rows >> c >> r)) return true;
      if (cols && c != cols) {
        err = "mixed rect widths";
        return false;
      }
      cols = c;
      auto& m = rect[rows];
      auto it = m.find((uint32_t)p);
      if (it != m.end() && it->second != r) {
        err = "conflicting rect residue for " + std::to_string(p);
        return false;
      }
      m[(uint32_t)p] = (uint32_t)r;
    }
    return true;
  }
};

struct Recon {
  bool done = false;
  bool ok = false;
  Big x;
  size_t agree = 0;
  size_t total = 0;
};

inline Recon reconstruct(const std::map<uint32_t, uint32_t>& m) {
  Recon out;
  if (m.size() < 2) return out;
  std::vector<uint32_t> ps, rs;
  for (auto& e : m) {
    ps.push_back(e.first);
    rs.push_back(e.second);
  }
  std::vector<std::string> vals;
  for (size_t i = 0; i < ps.size(); ++i) {
    std::vector<uint32_t> p0, r0;
    for (size_t j = 0; j < ps.size(); ++j)
      if (j != i) {
        p0.push_back(ps[j]);
        r0.push_back(rs[j]);
      }
    Big x = crt(p0, r0);
    vals.push_back(x.dec());
    if (i == 0) out.x = x;
  }
  std::map<std::string, size_t> cnt;
  for (auto& v : vals) ++cnt[v];
  std::string best;
  size_t bc = 0;
  for (auto& e : cnt)
    if (e.second > bc) {
      bc = e.second;
      best = e.first;
    }
  for (size_t i = 0; i < vals.size(); ++i)
    if (vals[i] == best) {
      std::vector<uint32_t> p0, r0;
      for (size_t j = 0; j < ps.size(); ++j)
        if (j != i) {
          p0.push_back(ps[j]);
          r0.push_back(rs[j]);
        }
      out.x = crt(p0, r0);
      break;
    }
  out.done = true;
  out.agree = bc;
  out.total = vals.size();
  out.ok = bc == vals.size();
  return out;
}

inline int report(const Results& R, FILE* out, int squares) {
  Recon a = reconstruct(R.res);
  if (!a.done) return 0;
  std::string s = a.x.dec();
  size_t bits = 0;
  for (size_t i = a.x.d.size(); i-- > 0;)
    if (a.x.d[i]) {
      bits = i * 32 + 32 - __builtin_clz(a.x.d[i]);
      break;
    }
  bool even = a.x.d.empty() || (a.x.d[0] & 1) == 0;
  char line[256];
  std::snprintf(line, sizeof(line), "primes %zu leave_one_out %zu/%zu bits %zu even %s", a.total,
                a.agree, a.total, bits, (even || squares == 0) ? "yes" : "no");
  std::printf("%s\n", line);
  std::printf("digits %zu\n", s.size());
  std::printf("answer %s\n", s.c_str());
  std::printf("check %s\n", a.ok ? "ok" : "FAIL");
  if (out) std::fprintf(out, "# %s\n# answer %s\n", line, s.c_str());
  for (auto& e : R.rect) {
    if (e.second.size() != R.res.size()) continue;
    Recon b = reconstruct(e.second);
    if (!b.done) continue;
    std::string v = b.x.dec();
    std::printf("rect %d %d %s %s\n", e.first, R.cols, v.c_str(), b.ok ? "ok" : "FAIL");
    if (out) std::fprintf(out, "# rect %d %d %s %s\n", e.first, R.cols, v.c_str(), b.ok ? "ok" : "FAIL");
  }
  if (out) std::fprintf(out, "# check %s\n", a.ok ? "ok" : "FAIL");
  return a.ok ? 0 : 1;
}

struct Options {
  int n = 0;
  int k = 0;
  int lanes = 1;
  int first = 0;
  int last = -1;
  int gpus = 0;
  int bits = 16;
  int variant = 1;
  int tpb = 512;
  int bps = 16;
  int r32 = -1;
  bool info = false;
  std::vector<std::string> crt_files;
  std::vector<std::string> resume_files;
  std::string out_file;
};

inline void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s -n SQUARES [-b 16|32] [-k PRIMES] [-l LANES] [-r FIRST:LAST] [-g GPUS] [-v 0|1] [-T THREADS] [-B BLOCKS_PER_SM] [-o FILE] [--resume FILE] [--info]\n"
               "       %s --crt FILE...\n",
               prog, prog);
  std::exit(2);
}

inline Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&]() -> const char* {
      if (i + 1 >= argc) usage(argv[0]);
      return argv[++i];
    };
    if (a == "-n") o.n = std::atoi(need());
    else if (a == "-k") o.k = std::atoi(need());
    else if (a == "-l") o.lanes = std::atoi(need());
    else if (a == "-g") o.gpus = std::atoi(need());
    else if (a == "-b") o.bits = std::atoi(need());
    else if (a == "-v") o.variant = std::atoi(need());
    else if (a == "-T") o.tpb = std::atoi(need());
    else if (a == "-B") o.bps = std::atoi(need());
    else if (a == "-r") {
      std::string v = need();
      size_t c = v.find(':');
      if (c == std::string::npos) usage(argv[0]);
      o.first = std::atoi(v.substr(0, c).c_str());
      o.last = std::atoi(v.substr(c + 1).c_str());
    } else if (a == "--info") o.info = true;
    else if (a == "--resume") o.resume_files.push_back(need());
    else if (a == "-o") o.out_file = need();
    else if (a == "--r32") o.r32 = std::atoi(need());
    else if (a == "--crt") {
      while (i + 1 < argc) o.crt_files.push_back(argv[++i]);
    } else usage(argv[0]);
  }
  return o;
}

inline int crt_main(const Options& o) {
  Results R;
  for (auto& fn : o.crt_files) {
    std::ifstream in(fn);
    std::string line, err;
    while (std::getline(in, line))
      if (!R.add(line, err)) {
        std::printf("%s\n", err.c_str());
        return 1;
      }
  }
  if (R.res.size() < 2) {
    std::printf("need at least 2 residues\n");
    return 1;
  }
  return report(R, nullptr, 1);
}

inline int app_main(int argc, char** argv, Runner& R) {
  Options o = parse(argc, argv);
  if (!o.crt_files.empty()) return crt_main(o);
  if (o.n < 1 || o.n + 1 > MAXW) usage(argv[0]);
  int W = o.n + 1;
  R.variant = o.variant;
  R.tpb = o.tpb;
  R.gpus = o.gpus;
  R.bps = o.bps;
  Tab T = make_tab(W);
  if (o.r32 >= 0 && o.r32 < T.r32) T.r32 = o.r32;
  if (o.bits != 16 && o.bits != 32) usage(argv[0]);
  int k = o.k > 0 ? o.k : default_prime_count(W, o.bits);
  int last = o.last < 0 ? k : o.last;
  if (o.first < 0 || last > k || o.first >= last) usage(argv[0]);
  int L = o.lanes;
  if (L != 1 && L != 2 && L != 4 && L != 8) usage(argv[0]);
  std::vector<std::string> head;
  head.push_back(std::string("# build commit ") + FK_COMMIT);
  head.push_back("# binary sha256 " + sha256_file("/proc/self/exe"));
  {
    std::string a = "# argv";
    for (int i = 0; i < argc; ++i) a += std::string(" ") + argv[i];
    head.push_back(a);
  }
  head.push_back("# start " + utc_now());
  head.push_back(std::string("# engine ") + R.name());
  for (auto& x : R.info) head.push_back(x);
  head.push_back("# squares " + std::to_string(o.n) + " vertices_per_row " + std::to_string(W));
  head.push_back("# states " + std::to_string(T.total) + " main " + std::to_string(T.nmain) +
                 " blocked " + std::to_string(T.nblk));
  head.push_back("# primes " + std::to_string(k) + " range " + std::to_string(o.first) + ":" +
                 std::to_string(last) + " lanes " + std::to_string(L) + " bits " +
                 std::to_string(o.bits) + " variant " + std::to_string(o.variant) + " tpb " +
                 std::to_string(o.tpb) + " bps " + std::to_string(o.bps));
  {
    char b[96];
    std::snprintf(b, sizeof(b), "# bytes_per_pass %.3e r32 %d", R.bytes_per_pass(T, L, o.bits), T.r32);
    head.push_back(b);
  }
  for (auto& x : head) std::printf("%s\n", x.c_str());
  std::fflush(stdout);
  if (o.info) return 0;
  std::vector<uint32_t> all = prime_list(k, o.bits);
  char tag[64];
  std::snprintf(tag, sizeof(tag), "# fukashigi n=%d bits=%d", o.n, o.bits);
  Results res;
  for (auto& fn : o.resume_files) {
    std::ifstream in(fn);
    std::string line, err;
    while (std::getline(in, line)) {
      if (line.rfind("# fukashigi ", 0) == 0 && line != tag) {
        std::fprintf(stderr, "%s is for a different run: %s\n", fn.c_str(), line.c_str());
        return 2;
      }
      if (!res.add(line, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
      }
    }
  }
  for (auto it = res.res.begin(); it != res.res.end();) {
    bool known = false;
    for (uint32_t q : all) known |= q == it->first;
    it = known ? std::next(it) : res.res.erase(it);
  }
  FILE* out = nullptr;
  if (!o.out_file.empty()) {
    out = std::fopen(o.out_file.c_str(), "a+");
    if (!out) {
      std::fprintf(stderr, "cannot open %s\n", o.out_file.c_str());
      return 2;
    }
    std::fseek(out, 0, SEEK_END);
    if (std::ftell(out) == 0) std::fprintf(out, "%s\n", tag);
    for (auto& x : head) std::fprintf(out, "%s\n", x.c_str());
    std::fflush(out);
  }
  auto sync = [&]() {
    if (!out) return;
    std::fflush(out);
    fsync(fileno(out));
  };
  std::vector<uint32_t> ps;
  for (int i = o.first; i < last; ++i)
    if (!res.res.count(all[i])) ps.push_back(all[i]);
  std::printf("# resumed %zu todo %zu\n", res.res.size(), ps.size());
  std::fflush(stdout);
  double t0 = now_sec();
  for (size_t g = 0; g < ps.size(); g += L) {
    std::vector<uint32_t> grp(ps.begin() + g, ps.begin() + std::min(ps.size(), g + L));
    size_t real = grp.size();
    while ((int)grp.size() < L) grp.push_back(grp.back());
    std::string started = utc_now();
    double t1 = now_sec();
    PassOut r = R.pass(T, grp, o.bits);
    double sec = now_sec() - t1;
    std::vector<std::string> lines;
    for (size_t l = 0; l < real; ++l) {
      for (size_t i = 0; i < r.fp.size(); ++i)
        lines.push_back("fingerprint " + std::to_string(grp[l]) + " " + std::to_string(i + 1) + " " +
                        std::to_string(r.fp[i][l][0]) + " " + std::to_string(r.fp[i][l][1]));
      for (size_t i = 0; i < r.rect.size(); ++i)
        lines.push_back("rect " + std::to_string(grp[l]) + " " + std::to_string(i + 1) + " " +
                        std::to_string(W) + " " + std::to_string(r.rect[i][l]));
      lines.push_back("residue " + std::to_string(grp[l]) + " " + std::to_string(r.res[l]));
    }
    char b[128];
    std::snprintf(b, sizeof(b), "# pass %zu start %s seconds %.3f", g / L, started.c_str(), sec);
    lines.push_back(b);
    std::string err;
    for (auto& x : lines) {
      std::printf("%s\n", x.c_str());
      res.add(x, err);
      if (out) std::fprintf(out, "%s\n", x.c_str());
    }
    sync();
    std::fflush(stdout);
  }
  std::printf("# total_seconds %.3f\n", now_sec() - t0);
  int rc = 0;
  bool complete = true;
  for (uint32_t p : all) complete &= res.res.count(p) > 0;
  if (complete && k >= 2) rc = report(res, out, o.n);
  if (out) {
    std::fprintf(out, "# finished %zu/%d %s\n", res.res.size(), k, utc_now().c_str());
    sync();
    std::fclose(out);
  }
  return rc;
}

}
