#define FK_COUNT 1
#include "../src/host.hpp"

int fk::fk_owner = 0;
uint64_t fk::fk_local = 0;
uint64_t fk::fk_remote = 0;

int main(int argc, char** argv) {
  int n = std::atoi(argv[1]);
  int G = std::atoi(argv[2]);
  int W = n + 1;
  fk::Tab T = fk::make_tab(W);
  fk::Mem<uint16_t> M;
  fk::mem_layout(M, T, G);
  std::vector<std::vector<uint16_t>> Am(G), Ab(G);
  for (int g = 0; g < G; ++g) {
    Am[g].assign(fk::span(T.nmain, M.mshard, g) + 1, 0);
    Ab[g].assign(fk::span(T.nblk, M.bshard, g) + 1, 0);
    M.pm[g] = Am[g].data();
    M.pb[g] = Ab[g].data();
  }
  uint32_t P[1] = {65521};
  uint32_t BM[1] = {fk::barrett(65521)};
  *M.at(fk::init_index(T), 1) = 1;
  for (int i = 0; i < W; ++i) {
    for (uint64_t t = 0; t < T.nblk; ++t) {
      fk::fk_owner = (int)(t / M.bshard);
      if (i > 0) fk::row_convert<uint16_t, 1>(T, M, P, BM, t);
    }
    for (int j = 1; j < W; ++j) {
      uint64_t l0 = fk::fk_local, r0 = fk::fk_remote;
      for (uint64_t t = 0; t < T.nblk; ++t) {
        fk::fk_owner = (int)(t / M.bshard);
        fk::group_step<uint16_t, 1>(T, M, P, BM, t, j);
      }
      if (i == W / 2)
        std::printf("j=%2d remote %.4f\n", j,
                    (double)(fk::fk_remote - r0) / (double)(fk::fk_local - l0 + fk::fk_remote - r0));
    }
  }
  std::printf("total remote %.4f residue %u\n",
              (double)fk::fk_remote / (double)(fk::fk_local + fk::fk_remote),
              (unsigned)*M.at(fk::answer_index(T), 1));
}
