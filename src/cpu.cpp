#include "host.hpp"

#include <omp.h>

namespace fk {

struct CpuRunner : Runner {
  const char* name() const override { return "cpu"; }

  template <typename CT, int K>
  PassOut run(const Tab& T, const std::vector<uint32_t>& ps) {
    const int W = T.W;
    const int G = std::max(1, std::min(MAXG, gpus));
    Mem<CT> M;
    mem_layout(M, T, G);
    std::vector<std::vector<CT>> Am(G), Ab(G);
    for (int g = 0; g < G; ++g) {
      if (G == 1) {
        Am[g].assign(T.total * K, 0);
      } else {
        Am[g].assign((span(T.nmain, M.mshard, g) + 1) * K, 0);
        Ab[g].assign((span(T.nblk, M.bshard, g) + 1) * K, 0);
        M.pb[g] = Ab[g].data();
      }
      M.pm[g] = Am[g].data();
    }
    const int V = variant;
    uint32_t P[MAXK], BM[MAXK];
    for (int l = 0; l < K; ++l) {
      P[l] = ps[l];
      BM[l] = barrett(ps[l]);
    }
    CT* s0 = M.at(init_index(T), K);
    for (int l = 0; l < K; ++l) s0[l] = 1;
    const int64_t NB = (int64_t)T.nblk;
    PassOut po;
    for (int i = 0; i < W; ++i) {
      double t0 = now_sec();
      if (i > 0) {
#pragma omp parallel for schedule(dynamic, 65536)
        for (int64_t t = 0; t < NB; ++t) row_convert<CT, K>(T, M, P, BM, (uint64_t)t);
      }
      for (int j = 1; j < W; ++j) {
#pragma omp parallel for schedule(dynamic, 65536)
        for (int64_t t = 0; t < NB; ++t) {
          if (V == 0) group_update<CT, K>(T, M, P, BM, (uint64_t)t, j);
          else group_step<CT, K>(T, M, P, BM, (uint64_t)t, j);
        }
      }
      {
        const CT* a = M.at(answer_index(T), K);
        std::vector<uint32_t> rr(K);
        for (int l = 0; l < K; ++l) rr[l] = a[l];
        po.rect.push_back(rr);
        std::vector<std::array<uint64_t, 2>> fp(K);
        for (int l = 0; l < K; ++l) {
          const uint64_t p = P[l];
          uint64_t s0 = 0, s1 = 0;
          const int64_t NT = (int64_t)T.total;
#pragma omp parallel for reduction(+ : s0, s1) schedule(static)
          for (int64_t r = 0; r < NT; ++r) {
            uint64_t v = M.at((uint64_t)r, K)[l];
            s0 += v;
            s1 += v * ((uint64_t)r % p) % p;
          }
          fp[l] = {s0 % p, s1 % p};
        }
        po.fp.push_back(fp);
      }
      std::fprintf(stderr, "row %d/%d %.3fs\n", i + 1, W, now_sec() - t0);
    }
    po.res = po.rect.back();
    return po;
  }

  template <typename CT>
  PassOut run_k(const Tab& T, const std::vector<uint32_t>& ps) {
    switch (ps.size()) {
      case 1: return run<CT, 1>(T, ps);
      case 2: return run<CT, 2>(T, ps);
      case 4: return run<CT, 4>(T, ps);
      default: return run<CT, 8>(T, ps);
    }
  }

  PassOut pass(const Tab& T, const std::vector<uint32_t>& ps, int bits) override {
    if (bits == 16) return run_k<uint16_t>(T, ps);
    return run_k<uint32_t>(T, ps);
  }
};

}

int main(int argc, char** argv) {
  fk::CpuRunner R;
  R.info.push_back("# device cpu threads " + std::to_string(omp_get_max_threads()));
  return fk::app_main(argc, argv, R);
}
