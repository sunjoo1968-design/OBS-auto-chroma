-- ac_tune_math.lua — ac_tune 의 순수 계산 헬퍼 (sRGB 변환 LUT·반올림·히스토그램 백분위·중앙값). OBS 의존 없음.
-- ac_tune.lua 만 require 한다(단방향). 값은 sim/autotune.py 와 같은 결과여야 하므로 수정 시 tests/lua/run_parity.sh 필수.
local ffi = require("ffi")

local M = {}

M.BYTES = 4
local SRGB_DECODE_CUTOFF, SRGB_LINEAR_CUTOFF, ENCODE_LUT_SIZE = 0.04045, 0.0031308, 8192
local DIST_BIN_WIDTH = 1e-5   -- 백분위용 히스토그램 폭 (오차 < 0.01 슬라이더 단위)
local DIST_BIN_COUNT = 200000 -- 거리 0~2.0
local BYTES = M.BYTES


-- ---- sRGB 변환 LUT ----
local function srgb_decode(v)
  if v <= SRGB_DECODE_CUTOFF then return v / 12.92 end
  return ((v + 0.055) / 1.055) ^ 2.4
end

local function srgb_encode(u)
  if u <= SRGB_LINEAR_CUTOFF then return 12.92 * u end
  return 1.055 * u ^ (1 / 2.4) - 0.055
end

local DECODE_LUT = ffi.new("double[256]")
for v = 0, 255 do DECODE_LUT[v] = srgb_decode(v / 255) end
local ENCODE_LUT = ffi.new("double[?]", ENCODE_LUT_SIZE + 2)
for i = 0, ENCODE_LUT_SIZE + 1 do ENCODE_LUT[i] = srgb_encode(i / ENCODE_LUT_SIZE) end

-- 선형 -> sRGB (LUT 선형보간)
local function encode(u)
  if u < 0 then u = 0 elseif u > 1 then u = 1 end
  local x = u * ENCODE_LUT_SIZE
  local i = math.floor(x)
  local a = ENCODE_LUT[i]
  return a + (ENCODE_LUT[i + 1] - a) * (x - i)
end
M.encode = encode

-- python round() 와 같은 반올림(.5는 짝수 쪽)
local function round_half_even(x)
  local f = math.floor(x)
  local d = x - f
  if d > 0.5 then return f + 1 end
  if d < 0.5 then return f end
  return (f % 2 == 0) and f or f + 1
end
M.round_half_even = round_half_even

-- 채널 히스토그램(hist[off..off+255])의 중앙값. 짝수 개면 가운데 두 값 평균 (numpy.median)
local function median_from_hist(hist, off, n)
  local lo_rank, hi_rank = math.floor((n - 1) / 2), math.floor(n / 2)
  local cum, lo_val, hi_val = 0, nil, nil
  for v = 0, 255 do
    cum = cum + hist[off + v]
    if lo_val == nil and cum > lo_rank then lo_val = v end
    if hi_val == nil and cum > hi_rank then hi_val = v; break end
  end
  return (lo_val + hi_val) / 2
end
M.median_from_hist = median_from_hist

-- 전체 이미지를 선형 공간 float 3면(R,G,B)으로 변환
local function build_linear_planes(rgba, w, h)
  local n = w * h
  local lr, lg, lb = ffi.new("float[?]", n), ffi.new("float[?]", n), ffi.new("float[?]", n)
  for p = 0, n - 1 do
    local i = p * BYTES
    lr[p], lg[p], lb[p] = DECODE_LUT[rgba[i]], DECODE_LUT[rgba[i + 1]], DECODE_LUT[rgba[i + 2]]
  end
  return lr, lg, lb
end
M.build_linear_planes = build_linear_planes

-- 부분집합 (lo < d <= hi) 의 선형보간 백분위 (numpy.percentile 기본값과 같은 방식). 빈 집합이면 nil
local function subset_percentiles(dist, n, lo, hi, qs)
  local hist = ffi.new("uint32_t[?]", DIST_BIN_COUNT)
  local total = 0
  for i = 0, n - 1 do
    local d = dist[i]
    if d > lo and d <= hi then
      local b = math.min(math.floor(d / DIST_BIN_WIDTH), DIST_BIN_COUNT - 1)
      hist[b] = hist[b] + 1
      total = total + 1
    end
  end
  if total == 0 then return nil end
  -- 정렬했을 때 k번째(0부터) 값의 근사: 칸 안에서 균등 분포 가정
  local function kth(k)
    local cum = 0
    for b = 0, DIST_BIN_COUNT - 1 do
      local c = hist[b]
      if cum + c > k then return (b + (k - cum + 0.5) / c) * DIST_BIN_WIDTH end
      cum = cum + c
    end
    return (DIST_BIN_COUNT - 1) * DIST_BIN_WIDTH
  end
  local out = {}
  for j, q in ipairs(qs) do
    local pos = q / 100 * (total - 1)
    local k = math.floor(pos)
    local f = pos - k
    out[j] = (f == 0) and kth(k) or kth(k) * (1 - f) + kth(math.min(k + 1, total - 1)) * f
  end
  return out
end

-- thr 초과(le=false) 또는 이하(le=true) 개수
local function count_dist(dist, n, thr, le)
  local c = 0
  for i = 0, n - 1 do if (dist[i] <= thr) == le then c = c + 1 end end
  return c
end
M.subset_percentiles = subset_percentiles
M.count_dist = count_dist

return M
