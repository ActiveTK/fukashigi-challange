#define FK_BOUNDS 1
#include "../src/host.hpp"

#include <algorithm>
#include <random>

static uint64_t g_total = 0;
static uint64_t g_bad = 0;
static uint16_t g_scratch[64];
static std::vector<uint64_t> g_rec;

template <typename CT>
CT* fk::fk_bounds_at(uint64_t r, int K) {
  if (r >= g_total) {
    if (g_bad < 10) std::printf("  out of range %llu >= %llu\n", (unsigned long long)r, (unsigned long long)g_total);
    ++g_bad;
  }
  (void)K;
  g_rec.push_back(r);
  return reinterpret_cast<CT*>(g_scratch);
}

int main(int argc, char** argv) {
  int lo = std::atoi(argv[1]);
  int hi = std::atoi(argv[2]);
  int samples = argc > 3 ? std::atoi(argv[3]) : 20000;
  std::mt19937_64 rng(12345);
  for (int n = lo; n <= hi; ++n) {
    int W = n + 1;
    fk::Tab T = fk::make_tab(W);
    g_total = T.total;
    g_bad = 0;
    fk::Mem<uint16_t> M;
    fk::mem_layout(M, T, 1);
    uint32_t P[1] = {65521};
    uint32_t BM[1] = {fk::barrett(65521)};
    uint64_t diff = 0;
    for (int j = 1; j < W; ++j) {
      for (int s = 0; s < samples; ++s) {
        uint64_t t = s < 64 ? (uint64_t)s : (s < 128 ? T.nblk - 1 - (uint64_t)(s - 64) : rng() % T.nblk);
        g_rec.clear();
        fk::group_update<uint16_t, 1>(T, M, P, BM, t, j);
        std::vector<uint64_t> a = g_rec;
        g_rec.clear();
        fk::group_step<uint16_t, 1>(T, M, P, BM, t, j);
        std::vector<uint64_t> b = g_rec;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
        b.erase(std::unique(b.begin(), b.end()), b.end());
        if (a != b) ++diff;
      }
    }
    for (int s = 0; s < samples; ++s) {
      uint64_t t = rng() % T.nblk;
      fk::row_convert<uint16_t, 1>(T, M, P, BM, t);
    }
    std::printf("n=%d W=%d r32=%d total=%llu bad=%llu diff=%llu\n", n, W, T.r32,
                (unsigned long long)T.total, (unsigned long long)g_bad, (unsigned long long)diff);
  }
}
