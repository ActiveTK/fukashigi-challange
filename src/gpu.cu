#include "host.hpp"

#include <cuda_runtime.h>

#ifndef FK_MINB
#define FK_MINB 1
#endif

#define CK(x)                                                                              \
  do {                                                                                     \
    cudaError_t e_ = (x);                                                                  \
    if (e_ != cudaSuccess) {                                                               \
      std::fprintf(stderr, "cuda error %s at %s:%d\n", cudaGetErrorString(e_), __FILE__,   \
                   __LINE__);                                                              \
      std::exit(1);                                                                        \
    }                                                                                      \
  } while (0)

namespace fk {

__constant__ uint32_t cP[MAXK];
__constant__ uint32_t cM[MAXK];

__device__ __forceinline__ void load_tab(const Tab* gT, Tab& sT) {
  const uint64_t* a = reinterpret_cast<const uint64_t*>(gT);
  uint64_t* b = reinterpret_cast<uint64_t*>(&sT);
  for (int w = threadIdx.x; w < (int)(sizeof(Tab) / 8); w += blockDim.x) b[w] = a[w];
  __syncthreads();
}

template <typename CT, int K, int V>
__global__ void __launch_bounds__(512, FK_MINB) step_kernel(const Tab* __restrict__ gT, Mem<CT> M,
                                                            uint64_t lo, uint64_t hi, int j) {
  __shared__ Tab sT;
  load_tab(gT, sT);
  const uint64_t stride = (uint64_t)gridDim.x * blockDim.x;
  for (uint64_t t = lo + (uint64_t)blockIdx.x * blockDim.x + threadIdx.x; t < hi; t += stride) {
    if (V == 0) group_update<CT, K>(sT, M, cP, cM, t, j);
    else group_step<CT, K>(sT, M, cP, cM, t, j);
  }
}

template <typename CT, int K>
__global__ void __launch_bounds__(256) fingerprint_kernel(const CT* __restrict__ base, uint64_t count,
                                                         uint64_t gstart,
                                                         unsigned long long* __restrict__ acc) {
  uint64_t s0[K], s1[K];
  uint32_t w[K];
#pragma unroll
  for (int l = 0; l < K; ++l) {
    s0[l] = 0;
    s1[l] = 0;
  }
  const uint64_t stride = (uint64_t)gridDim.x * blockDim.x;
  int pend = 0;
  for (uint64_t t = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x; t < count; t += stride) {
    const uint64_t r = gstart + t;
#pragma unroll
    for (int l = 0; l < K; ++l) {
      w[l] = (uint32_t)(r % cP[l]);
      uint64_t v = base[t * K + l];
      s0[l] += v;
      s1[l] += v * w[l];
    }
    if (++pend == 256) {
      pend = 0;
#pragma unroll
      for (int l = 0; l < K; ++l) {
        s0[l] %= cP[l];
        s1[l] %= cP[l];
      }
    }
  }
#pragma unroll
  for (int l = 0; l < K; ++l) {
    atomicAdd(&acc[2 * l], (unsigned long long)(s0[l] % cP[l]));
    atomicAdd(&acc[2 * l + 1], (unsigned long long)(s1[l] % cP[l]));
  }
}

template <typename CT, int K>
__global__ void __launch_bounds__(512, FK_MINB) convert_kernel(const Tab* __restrict__ gT, Mem<CT> M,
                                                      uint64_t lo, uint64_t hi) {
  __shared__ Tab sT;
  load_tab(gT, sT);
  const uint64_t stride = (uint64_t)gridDim.x * blockDim.x;
  for (uint64_t t = lo + (uint64_t)blockIdx.x * blockDim.x + threadIdx.x; t < hi; t += stride)
    row_convert<CT, K>(sT, M, cP, cM, t);
}

struct GpuRunner : Runner {
  int G = 0;

  const char* name() const override { return "cuda"; }

  void setup(int want) {
    int n = 0;
    CK(cudaGetDeviceCount(&n));
    if (n == 0) {
      std::fprintf(stderr, "no cuda device\n");
      std::exit(1);
    }
    G = want > 0 ? std::min(want, n) : n;
    if (G > MAXG) G = MAXG;
    for (int a = 0; a < G; ++a) {
      cudaDeviceProp p;
      CK(cudaGetDeviceProperties(&p, a));
      char line[256];
      std::snprintf(line, sizeof(line), "# gpu %d %s %.1f GB sm %d cc %d.%d pci %04x:%02x:%02x", a,
                    p.name, p.totalGlobalMem / 1e9, p.multiProcessorCount, p.major, p.minor,
                    p.pciDomainID, p.pciBusID, p.pciDeviceID);
      info.push_back(line);
    }
    {
      int drv = 0, rt = 0;
      cudaDriverGetVersion(&drv);
      cudaRuntimeGetVersion(&rt);
      info.push_back("# cuda driver " + std::to_string(drv) + " runtime " + std::to_string(rt));
    }
    for (int a = 0; a < G; ++a) {
      CK(cudaSetDevice(a));
      for (int b = 0; b < G; ++b) {
        if (a == b) continue;
        int ok = 0;
        CK(cudaDeviceCanAccessPeer(&ok, a, b));
        if (!ok) {
          std::fprintf(stderr, "gpu %d cannot access gpu %d\n", a, b);
          std::exit(1);
        }
        cudaError_t e = cudaDeviceEnablePeerAccess(b, 0);
        if (e != cudaSuccess && e != cudaErrorPeerAccessAlreadyEnabled) CK(e);
        cudaGetLastError();
      }
    }
    std::fflush(stdout);
  }

  void sync_all() {
    for (int g = 0; g < G; ++g) {
      CK(cudaSetDevice(g));
      CK(cudaDeviceSynchronize());
    }
  }

  template <typename CT, int K>
  PassOut run(const Tab& T, const std::vector<uint32_t>& ps) {
    const int W = T.W;
    const uint64_t N = T.total;
    Mem<CT> M;
    mem_layout(M, T, G);
    std::vector<Tab*> dT(G);
    std::vector<unsigned long long*> acc(G, nullptr);
    PassOut po;
    std::vector<int> blocks(G);
    std::vector<uint64_t> lo(G), hi(G);
    uint32_t bm[MAXK];
    for (int l = 0; l < K; ++l) bm[l] = barrett(ps[l]);
    for (int g = 0; g < G; ++g) {
      lo[g] = std::min(T.nblk, (uint64_t)g * M.bshard);
      hi[g] = G == 1 ? T.nblk : std::min(T.nblk, (uint64_t)(g + 1) * M.bshard);
      CK(cudaSetDevice(g));
      const size_t mbytes = (size_t)((G == 1 ? N : span(T.nmain, M.mshard, g)) + 1) * K * sizeof(CT);
      const size_t bbytes = G == 1 ? 0 : (size_t)(span(T.nblk, M.bshard, g) + 1) * K * sizeof(CT);
      size_t fr = 0, tot = 0;
      CK(cudaMemGetInfo(&fr, &tot));
      if (mbytes + bbytes + (64u << 20) > fr) {
        std::fprintf(stderr, "gpu %d needs %.2f GB, free %.2f GB\n", g, (mbytes + bbytes) / 1e9,
                     fr / 1e9);
        std::exit(1);
      }
      CK(cudaMalloc(&M.pm[g], mbytes));
      CK(cudaMemset(M.pm[g], 0, mbytes));
      if (bbytes) {
        CK(cudaMalloc(&M.pb[g], bbytes));
        CK(cudaMemset(M.pb[g], 0, bbytes));
      }
      CK(cudaMalloc(&dT[g], sizeof(Tab)));
      CK(cudaMalloc(&acc[g], sizeof(unsigned long long) * 2 * K));
      CK(cudaMemcpy(dT[g], &T, sizeof(Tab), cudaMemcpyHostToDevice));
      CK(cudaMemcpyToSymbol(cP, ps.data(), 4 * K));
      CK(cudaMemcpyToSymbol(cM, bm, 4 * K));
      cudaDeviceProp p;
      CK(cudaGetDeviceProperties(&p, g));
      blocks[g] = p.multiProcessorCount * bps;
    }
    {
      std::vector<CT> one(K, 1);
      CK(cudaMemcpy(M.at(init_index(T), K), one.data(), sizeof(CT) * K, cudaMemcpyDefault));
    }
    for (int i = 0; i < W; ++i) {
      double t0 = now_sec();
      if (i > 0) {
        for (int g = 0; g < G; ++g) {
          if (lo[g] >= hi[g]) continue;
          CK(cudaSetDevice(g));
          convert_kernel<CT, K><<<blocks[g], tpb>>>(dT[g], M, lo[g], hi[g]);
          CK(cudaGetLastError());
        }
        sync_all();
      }
      for (int j = 1; j < W; ++j) {
        for (int g = 0; g < G; ++g) {
          if (lo[g] >= hi[g]) continue;
          CK(cudaSetDevice(g));
          if (variant == 0)
            step_kernel<CT, K, 0><<<blocks[g], tpb>>>(dT[g], M, lo[g], hi[g], j);
          else
            step_kernel<CT, K, 1><<<blocks[g], tpb>>>(dT[g], M, lo[g], hi[g], j);
          CK(cudaGetLastError());
        }
        sync_all();
      }
      {
        std::vector<CT> raw(K);
        CK(cudaMemcpy(raw.data(), M.at(answer_index(T), K), sizeof(CT) * K, cudaMemcpyDefault));
        po.rect.push_back(std::vector<uint32_t>(raw.begin(), raw.end()));
        std::vector<std::array<uint64_t, 2>> fp(K, {0, 0});
        for (int g = 0; g < G; ++g) {
          CK(cudaSetDevice(g));
          CK(cudaMemset(acc[g], 0, sizeof(unsigned long long) * 2 * K));
          uint64_t mc = G == 1 ? N : span(T.nmain, M.mshard, g);
          if (mc)
            fingerprint_kernel<CT, K><<<blocks[g], 256>>>(M.pm[g], mc, (uint64_t)g * M.mshard, acc[g]);
          uint64_t bc = G == 1 ? 0 : span(T.nblk, M.bshard, g);
          if (bc)
            fingerprint_kernel<CT, K><<<blocks[g], 256>>>(M.pb[g], bc, T.nmain + (uint64_t)g * M.bshard,
                                                         acc[g]);
          CK(cudaGetLastError());
        }
        sync_all();
        for (int g = 0; g < G; ++g) {
          std::vector<unsigned long long> h(2 * K);
          CK(cudaSetDevice(g));
          CK(cudaMemcpy(h.data(), acc[g], sizeof(unsigned long long) * 2 * K, cudaMemcpyDeviceToHost));
          for (int l = 0; l < K; ++l) {
            fp[l][0] = (fp[l][0] + h[2 * l] % ps[l]) % ps[l];
            fp[l][1] = (fp[l][1] + h[2 * l + 1] % ps[l]) % ps[l];
          }
        }
        po.fp.push_back(fp);
      }
      std::fprintf(stderr, "row %d/%d %.3fs\n", i + 1, W, now_sec() - t0);
    }
    for (int g = 0; g < G; ++g) {
      CK(cudaSetDevice(g));
      CK(cudaFree(M.pm[g]));
      if (M.pb[g]) CK(cudaFree(M.pb[g]));
      CK(cudaFree(dT[g]));
      CK(cudaFree(acc[g]));
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
  fk::Options o = fk::parse(argc, argv);
  fk::GpuRunner R;
  if (o.crt_files.empty() && !o.info) R.setup(o.gpus);
  return fk::app_main(argc, argv, R);
}
