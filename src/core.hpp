#pragma once
#include <cstdint>

#if defined(__CUDACC__)
#define HD __host__ __device__ __forceinline__
#else
#define HD inline
#endif

namespace fk {

constexpr int MAXW = 31;
constexpr int MAXG = 16;
constexpr int MAXK = 8;

struct Tab {
  int W;
  uint64_t nmain;
  uint64_t nblk;
  uint64_t total;
  int r32;
  uint64_t f[MAXW + 1][MAXW + 6];
  uint32_t f32[MAXW + 1][MAXW + 6];
};

constexpr int SHARD_SHIFT = 12;

#if defined(FK_COUNT)
extern int fk_owner;
extern uint64_t fk_local;
extern uint64_t fk_remote;
#define FK_TOUCH(g) ((int)(g) == fk_owner ? ++fk_local : ++fk_remote)
#else
#define FK_TOUCH(g) ((void)0)
#endif

#if defined(FK_BOUNDS)
template <typename CT>
CT* fk_bounds_at(uint64_t r, int K);
#endif

template <typename CT>
struct Mem {
  CT* pm[MAXG];
  CT* pb[MAXG];
  uint64_t nmain;
  uint64_t mshard;
  uint64_t bshard;
  uint32_t mu;
  uint32_t bu;
  int G;
  HD CT* at(uint64_t r, int K) const {
#if defined(FK_BOUNDS)
    return fk_bounds_at<CT>(r, K);
#endif
    if (G == 1) return pm[0] + r * (uint64_t)K;
    if (r < nmain) {
      uint32_t g = (uint32_t)(r >> SHARD_SHIFT) / mu;
      FK_TOUCH(g);
      return pm[g] + (r - (uint64_t)g * mshard) * (uint64_t)K;
    }
    r -= nmain;
    uint32_t g = (uint32_t)(r >> SHARD_SHIFT) / bu;
    FK_TOUCH(g);
    return pb[g] + (r - (uint64_t)g * bshard) * (uint64_t)K;
  }
};

inline uint64_t shard_size(uint64_t total, int G) {
  uint64_t unit = 1ull << SHARD_SHIFT;
  uint64_t per = (total + G - 1) / G;
  return (per + unit - 1) / unit * unit;
}

inline uint64_t span(uint64_t n, uint64_t shard, int g) {
  uint64_t lo = (uint64_t)g * shard;
  if (lo >= n) return 0;
  return n - lo < shard ? n - lo : shard;
}

template <typename CT>
inline void mem_layout(Mem<CT>& M, const Tab& T, int G) {
  M.G = G;
  M.nmain = T.nmain;
  M.mshard = G == 1 ? T.total : shard_size(T.nmain, G);
  M.bshard = G == 1 ? T.nblk : shard_size(T.nblk, G);
  M.mu = (uint32_t)(M.mshard >> SHARD_SHIFT);
  M.bu = (uint32_t)(M.bshard >> SHARD_SHIFT);
  for (int g = 0; g < MAXG; ++g) {
    M.pm[g] = nullptr;
    M.pb[g] = nullptr;
  }
}

HD int sym(uint64_t c, int i) { return (int)((c >> (2 * i)) & 3u); }

HD uint64_t put(uint64_t c, int i, int s) {
  return (c & ~(3ull << (2 * i))) | ((uint64_t)s << (2 * i));
}

HD int dh(int s) { return (s == 1) - (s == 2); }

HD uint64_t rank_str(const Tab& T, uint64_t c, int len, int skip) {
  uint64_t r = 0;
  int h = 1;
  int rem = len;
  for (int i = 0; i < T.W; ++i) {
    if (i == skip) continue;
    --rem;
    int s = sym(c, i);
    if (s >= 1) r += T.f[rem][h];
    if (s >= 2) r += T.f[rem][h + 1];
    h += dh(s);
  }
  return r;
}

HD uint64_t unrank_str(const Tab& T, uint64_t idx, int len, int skip) {
  uint64_t c = 0;
  int h = 1;
  int rem = len;
  for (int i = 0; i < T.W; ++i) {
    if (i == skip) continue;
    --rem;
    uint64_t c0 = T.f[rem][h];
    if (idx < c0) continue;
    idx -= c0;
    uint64_t c1 = T.f[rem][h + 1];
    if (idx < c1) {
      c |= 1ull << (2 * i);
      ++h;
    } else {
      idx -= c1;
      c |= 2ull << (2 * i);
      --h;
    }
  }
  return c;
}

HD uint64_t rank_state(const Tab& T, uint64_t c, int bpos) {
  if (sym(c, bpos) == 3) return T.nmain + rank_str(T, c, T.W - 1, bpos);
  return rank_str(T, c, T.W, -1);
}

HD uint64_t unrank_state(const Tab& T, uint64_t idx, int bpos) {
  if (idx < T.nmain) return unrank_str(T, idx, T.W, -1);
  return put(unrank_str(T, idx - T.nmain, T.W - 1, bpos), bpos, 3);
}

HD uint64_t mid(const Tab& T, int j, int h, int a, int b) {
  int r1 = T.W - j;
  int r2 = T.W - 1 - j;
  uint64_t r = 0;
  if (a >= 1) r += T.f[r1][h];
  if (a >= 2) r += T.f[r1][h + 1];
  int h1 = h + dh(a);
  if (b >= 1) r += T.f[r2][h1];
  if (b >= 2) r += T.f[r2][h1 + 1];
  return r;
}

HD uint32_t reduce_mod(uint32_t v, uint32_t p, uint32_t m) {
  uint32_t q = (uint32_t)(((uint64_t)v * m) >> 32);
  uint32_t r = v - q * p;
  if (r >= p) r -= p;
  if (r >= p) r -= p;
  return r;
}

HD uint32_t barrett(uint32_t p) { return 0xFFFFFFFFu / p; }

template <typename CT, int K>
HD void group_update(const Tab& T, const Mem<CT>& M, const uint32_t* P, const uint32_t* BM,
                     uint64_t tau, int j) {
  const int W = T.W;
  const uint64_t bidx = T.nmain + tau;
  uint64_t code = put(unrank_str(T, tau, W - 1, j), j, 0);
  int c = sym(code, j - 1);
  int h = 1;
  for (int i = 0; i < j - 1; ++i) h += dh(sym(code, i));
  uint64_t base = rank_str(T, code, W, -1) - mid(T, j, h, c, 0);
  CT* pb = M.at(bidx, K);
  if (c == 0) {
    CT* p00 = M.at(base, K);
    CT* pLR = M.at(base + mid(T, j, h, 1, 2), K);
    uint32_t acc[K];
#pragma unroll
    for (int l = 0; l < K; ++l) acc[l] = 0;
    if (h >= 1) {
      const CT* pRL = M.at(base + mid(T, j, h, 2, 1), K);
#pragma unroll
      for (int l = 0; l < K; ++l) acc[l] = pRL[l];
    }
    {
      int d = 0;
      for (int q = j + 1; q < W; ++q) {
        int s = sym(code, q);
        if (s == 1) {
          if (d == 0) {
            uint64_t src = put(put(put(code, j - 1, 1), j, 1), q, 2);
            const CT* ps = M.at(rank_str(T, src, W, -1), K);
#pragma unroll
            for (int l = 0; l < K; ++l) acc[l] += ps[l];
          }
          ++d;
        } else if (s == 2) {
          if (d == 0) break;
          --d;
        }
      }
    }
    {
      int d = 0;
      for (int q = j - 2; q >= 0; --q) {
        int s = sym(code, q);
        if (s == 2) {
          if (d == 0) {
            uint64_t src = put(put(put(code, j - 1, 2), j, 2), q, 1);
            const CT* ps = M.at(rank_str(T, src, W, -1), K);
#pragma unroll
            for (int l = 0; l < K; ++l) acc[l] += ps[l];
          }
          ++d;
        } else if (s == 1) {
          if (d == 0) break;
          --d;
        }
      }
    }
#pragma unroll
    for (int l = 0; l < K; ++l) {
      uint32_t o00 = p00[l];
      uint32_t ob = pb[l];
      uint32_t oLR = pLR[l];
      p00[l] = (CT)reduce_mod(o00 + ob, P[l], BM[l]);
      pLR[l] = (CT)reduce_mod(oLR + o00, P[l], BM[l]);
      pb[l] = (CT)reduce_mod(acc[l], P[l], BM[l]);
    }
  } else {
    CT* p0X = M.at(base + mid(T, j, h, 0, c), K);
    const CT* pX0 = M.at(base + mid(T, j, h, c, 0), K);
#pragma unroll
    for (int l = 0; l < K; ++l) {
      uint32_t o0X = p0X[l];
      uint32_t ob = pb[l];
      uint32_t oX0 = pX0[l];
      p0X[l] = (CT)reduce_mod(o0X + ob + oX0, P[l], BM[l]);
      pb[l] = (CT)o0X;
    }
  }
}

struct Dec {
  uint64_t idx;
  uint64_t code;
  uint64_t pm;
  uint64_t pr;
  int h;
  int hj;
  int rem;
};

HD Dec dec_init(const Tab& T, uint64_t tau) {
  Dec d;
  d.idx = tau;
  d.code = 0;
  d.pm = 0;
  d.pr = 0;
  d.h = 1;
  d.hj = 1;
  d.rem = T.W - 1;
  return d;
}

template <typename U>
HD U tf(const Tab& T, int r, int h);

template <>
HD uint32_t tf<uint32_t>(const Tab& T, int r, int h) {
  return T.f32[r][h];
}

template <>
HD uint64_t tf<uint64_t>(const Tab& T, int r, int h) {
  return T.f[r][h];
}

HD Dec dec_prefix(const Tab& T, int j, uint64_t tau) {
  Dec d = dec_init(T, tau);
  int i = 0;
  for (; i < j; ++i) {
    if (d.rem < T.r32) break;
    --d.rem;
    if (i == j - 1) d.hj = d.h;
    uint64_t c0 = T.f[d.rem][d.h];
    if (d.idx < c0) continue;
    d.idx -= c0;
    uint64_t c1 = T.f[d.rem][d.h + 1];
    int s;
    uint64_t used;
    if (d.idx < c1) {
      s = 1;
      used = c0;
    } else {
      d.idx -= c1;
      s = 2;
      used = c0 + c1;
    }
    if (i < j - 1) {
      d.pr += used;
      d.pm += T.f[d.rem + 1][d.h];
      if (s == 2) d.pm += T.f[d.rem + 1][d.h + 1];
    }
    d.code |= (uint64_t)s << (2 * i);
    d.h += dh(s);
  }
  uint32_t ix = (uint32_t)d.idx;
  uint32_t pm = 0;
  uint32_t pr = 0;
  uint32_t lo = 0;
  uint32_t hi = 0;
  int h = d.h;
  int hj = d.hj;
  int rem = d.rem;
  const bool narrow = i < j;
  for (; i < j; ++i) {
    --rem;
    if (i == j - 1) hj = h;
    const uint32_t c0 = T.f32[rem][h];
    if (ix < c0) continue;
    ix -= c0;
    const uint32_t c1 = T.f32[rem][h + 1];
    int s;
    uint32_t used;
    if (ix < c1) {
      s = 1;
      used = c0;
    } else {
      ix -= c1;
      s = 2;
      used = c0 + c1;
    }
    if (i < j - 1) {
      pr += used;
      pm += T.f32[rem + 1][h];
      if (s == 2) pm += T.f32[rem + 1][h + 1];
    }
    if (i < 16) lo |= (uint32_t)s << (2 * i);
    else hi |= (uint32_t)s << (2 * (i - 16));
    h += dh(s);
  }
  if (narrow) {
    d.code |= (uint64_t)lo | ((uint64_t)hi << 32);
    d.pm += pm;
    d.pr += pr;
    d.idx = ix;
    d.h = h;
    d.hj = hj;
    d.rem = rem;
  }
  return d;
}

template <typename U, typename CT, int K>
HD void flips_right(const Tab& T, const Mem<CT>& M, U idx, int rem, int j, int h, uint64_t head,
                    uint64_t suf, uint32_t* acc) {
  const int W = T.W;
  U a0 = 0;
  U a2 = 0;
  int d = 0;
  int hh = h;
  for (int q = j + 1; q < W; ++q) {
    --rem;
    int s = 0;
    {
      const U c0 = tf<U>(T, rem, hh);
      if (idx >= c0) {
        idx -= c0;
        const U c1 = tf<U>(T, rem, hh + 1);
        if (idx < c1) {
          s = 1;
        } else {
          idx -= c1;
          s = 2;
        }
      }
    }
    int r = W - 1 - q;
    U ct = 0;
    U c2 = 0;
    if (s >= 1) {
      ct = tf<U>(T, r, hh);
      c2 = tf<U>(T, r, hh + 2);
    }
    if (s == 2) {
      ct += tf<U>(T, r, hh + 1);
      c2 += tf<U>(T, r, hh + 3);
    }
    if (s == 1 && d == 0) {
      uint64_t rk = head + (uint64_t)a2 + T.f[r][h + 2] + T.f[r][h + 3] + (suf - a0 - ct);
      const CT* ps = M.at(rk, K);
#pragma unroll
      for (int l = 0; l < K; ++l) acc[l] += ps[l];
    }
    a0 += ct;
    a2 += c2;
    if (s == 1) {
      ++d;
    } else if (s == 2) {
      if (d == 0) break;
      --d;
    }
    hh += dh(s);
  }
}

template <typename U, typename CT, int K>
HD void flips_left(const Tab& T, const Mem<CT>& M, uint64_t code, int j, int h, uint64_t pm,
                   uint64_t tail, uint32_t* acc) {
  const int W = T.W;
  U b0 = 0;
  U b2 = 0;
  int d = 0;
  int ha = h;
  for (int q = j - 2; q >= 0; --q) {
    int s = sym(code, q);
    int r = W - 1 - q;
    int hb = ha - dh(s);
    U ct = 0;
    U c2 = 0;
    U f0 = 0;
    if (s >= 1) {
      f0 = tf<U>(T, r, hb);
      ct = f0;
      c2 = tf<U>(T, r, hb + 2);
    }
    if (s == 2) {
      ct += tf<U>(T, r, hb + 1);
      c2 += tf<U>(T, r, hb + 3);
    }
    if (s == 2 && d == 0) {
      uint64_t rk = (pm - b0 - ct) + (uint64_t)f0 + b2 + tail;
      const CT* ps = M.at(rk, K);
#pragma unroll
      for (int l = 0; l < K; ++l) acc[l] += ps[l];
    }
    b0 += ct;
    b2 += c2;
    if (s == 2) {
      ++d;
    } else if (s == 1) {
      if (d == 0) break;
      --d;
    }
    ha = hb;
  }
}

template <typename CT, int K>
HD void group_apply(const Tab& T, const Mem<CT>& M, const uint32_t* P, const uint32_t* BM,
                         uint64_t tau, int j, const Dec& d) {
  const int W = T.W;
  const uint64_t code = d.code;
  const uint64_t pm = d.pm;
  const int h = d.hj;
  const int c = sym(code, j - 1);
  uint64_t rj = 0;
  if (c >= 1) rj += T.f[W - 1 - j][h];
  if (c >= 2) rj += T.f[W - 1 - j][h + 1];
  const uint64_t suf = tau - d.pr - rj;
  const uint64_t base = pm + suf;
  CT* pb = M.at(T.nmain + tau, K);
  if (c == 0) {
    CT* p00 = M.at(base, K);
    CT* pLR = M.at(base + mid(T, j, h, 1, 2), K);
    uint32_t acc[K];
#pragma unroll
    for (int l = 0; l < K; ++l) acc[l] = 0;
    if (h >= 1) {
      const CT* pRL = M.at(base + mid(T, j, h, 2, 1), K);
#pragma unroll
      for (int l = 0; l < K; ++l) acc[l] = pRL[l];
    }
    const uint64_t head = pm + mid(T, j, h, 1, 1);
    if (W - 1 - j <= T.r32 - 1)
      flips_right<uint32_t, CT, K>(T, M, (uint32_t)d.idx, d.rem, j, h, head, suf, acc);
    else
      flips_right<uint64_t, CT, K>(T, M, d.idx, d.rem, j, h, head, suf, acc);
    const uint64_t tail = mid(T, j, h + 2, 2, 2) + suf;
    if (W <= T.r32) flips_left<uint32_t, CT, K>(T, M, code, j, h, pm, tail, acc);
    else flips_left<uint64_t, CT, K>(T, M, code, j, h, pm, tail, acc);
#pragma unroll
    for (int l = 0; l < K; ++l) {
      uint32_t o00 = p00[l];
      uint32_t ob = pb[l];
      uint32_t oLR = pLR[l];
      p00[l] = (CT)reduce_mod(o00 + ob, P[l], BM[l]);
      pLR[l] = (CT)reduce_mod(oLR + o00, P[l], BM[l]);
      pb[l] = (CT)reduce_mod(acc[l], P[l], BM[l]);
    }
  } else {
    CT* p0X = M.at(base + mid(T, j, h, 0, c), K);
    const CT* pX0 = M.at(base + mid(T, j, h, c, 0), K);
#pragma unroll
    for (int l = 0; l < K; ++l) {
      uint32_t o0X = p0X[l];
      uint32_t ob = pb[l];
      uint32_t oX0 = pX0[l];
      p0X[l] = (CT)reduce_mod(o0X + ob + oX0, P[l], BM[l]);
      pb[l] = (CT)o0X;
    }
  }
}

template <typename CT, int K>
HD void group_step(const Tab& T, const Mem<CT>& M, const uint32_t* P, const uint32_t* BM,
                   uint64_t tau, int j) {
  Dec d = dec_prefix(T, j, tau);
  group_apply<CT, K>(T, M, P, BM, tau, j, d);
}

template <typename CT, int K>
HD void row_convert(const Tab& T, const Mem<CT>& M, const uint32_t* P, const uint32_t* BM,
                    uint64_t tau) {
  CT* pb = M.at(T.nmain + tau, K);
  bool any = false;
#pragma unroll
  for (int l = 0; l < K; ++l) any |= pb[l] != 0;
  if (!any) return;
  uint64_t code = unrank_str(T, tau, T.W - 1, T.W - 1);
  CT* pm = M.at(rank_str(T, code, T.W, -1), K);
#pragma unroll
  for (int l = 0; l < K; ++l) {
    pm[l] = (CT)reduce_mod((uint32_t)pm[l] + pb[l], P[l], BM[l]);
    pb[l] = 0;
  }
}

}
