-- ac_plate_math.lua — 배경판(clean plate) 순수 계산: 프레임 누적 -> E·σ -> RGBA8 묶음, CPU 판정·점검. OBS 의존 없음.
-- 정답 원본: sim/plate_key.py (D7). 수정 시 tests/lua/run_parity.sh 필수(E·σ 바이트 일치, 점검 비율 <= 1e-4).
-- 판 = RGBA8: rgb = E(sRGB), a = σ * 16 * 255. 계산은 모두 선형 공간(셰이더도 판을 set_texture_srgb 로 올려 선형으로 비교).
-- 상수는 sim/constants.py PLATE_* 와 같은 값이어야 한다(한 줄 한 선언, 상수 정합 시험 대상).
-- ac_plate(저장·불러오기), ac_plate_keyer(셰이더 파라미터 기본값)가 이 모듈을 require 한다(단방향).
local ffi = require("ffi")
local tune_math = require("ac_tune_math")  -- 반올림(round_half_even)만 공용으로 쓴다

local M = {}

local PLATE_SIGMA_FLOOR = 0.00784313725490196  -- 잡음 σ 하한(선형 단위) = 2/255
local PLATE_SIGMA_SCALE = 16.0                 -- 판 알파 바이트 = σ * SCALE * 255
local PLATE_TAU_CD = 3.0
local PLATE_W_CD = 1.0
local PLATE_TAU_LO = 0.55
local PLATE_TAU_HI = 1.10
local PLATE_W_A = 0.05
local PLATE_CHECK_MAX = 0.02                   -- 사람 없는 화면의 전경 비율 상한
local PLATE_CHECK_ALPHA = 0.5                  -- 점검에서 전경으로 세는 알파 기준
local PLATE_EE_MIN = 1e-6                      -- E·E 이하(거의 검정)면 a=1
local PLATE_RETAKE_CD = 1.0                    -- 평균 CD 가 이보다 크면 '다시 찍기 권장'([17] 튜닝값, 0 이면 안내 끔)
-- D9 '같은 색 방향 밝기 변화 = 배경'(진한 유채색 판의 그림자 지우기). 정답: sim/plate_key.py _same_factor
local PLATE_SAME_ENABLE = true                 -- D9 스위치
local PLATE_SAME_A_LO = 0.35                   -- 밝기비 하한(이보다 어두우면 규칙 밖)
local PLATE_SAME_A_HI = 1.0                    -- 밝기비 상한
local PLATE_SAME_DC_MAX = 0.04                 -- 색도 거리 허용
local PLATE_SAME_DC_W = 0.01                   -- 색도 거리 전이 폭
local PLATE_SAME_CHROMA_MIN = 0.23             -- 판 채도 하한(이보다 옅은 판은 D9 끔)
local PLATE_SAME_GATE_W = 0.02                 -- 무채색 gate 전이 폭
local PLATE_CHROMA_SUM_MIN = 1e-6              -- 색도 v/(r+g+b) 합의 하한
local PLATE_CHROMA_GRAY = 1.0 / 3.0            -- 무채색 색도
-- D10 경계 색 되살리기(셰이더 전용 — Lua CPU 경로에는 쓰이지 않고 ac_plate_keyer 가 uniform 으로 주입). 정답: sim/plate_edge.py
local PLATE_EDGE_RING_PX = 3                   -- 둘레 표본 거리(px)
local PLATE_UNMIX_ALPHA_MIN = 0.7              -- 색 되살리기 나눗셈의 알파 하한
local PLATE_UNMIX_CLAMP_FHAT = true            -- 되살린 색을 채널별로 [min(I,F̂), max(I,F̂)] 안으로 제한(무지개 테 억제)
local PLATE_EDGE_BOUNDARY_ALPHA = 0.5          -- 경계 판정: 둘레 알파 최소가 이보다 작으면 경계
local PLATE_UNMIX_DENOM_MIN = 1e-6             -- 투영 α 분모(‖F̂-E‖²) 하한
local PLATE_EDGE_TIE_EPS = 1e-6                -- F̂ 선택 동점 시 CD 가중
local PLATE_KEEP_ALPHA_MAX = 0.5 / 255.0       -- D16: 판 알파가 이보다 작으면(바이트 0) 셰이더가 원본 보존

local SRGB_DECODE_CUTOFF, SRGB_LINEAR_CUTOFF = 0.04045, 0.0031308
local RGB_MAX = 255.0
local RGBA_STRIDE, RGB_STRIDE = 4, 3
-- 박스 샘플: {dx, dy, 가중치}. 내장 v2 와 같은 오프셋(sim/constants.py BOX_SAMPLE_OFFSETS)
local BOX_SAMPLES = { { -1.0, -0.5, 2.0 }, { 1.0, 0.5, 2.0 }, { -0.5, 1.0, 2.0 }, { 0.5, -1.0, 2.0 }, { 0.0, 0.0, 1.0 } }
local BOX_WEIGHT_SUM = 9.0

M.PARAM_DEFAULTS = { tau_cd = PLATE_TAU_CD, w_cd = PLATE_W_CD, tau_lo = PLATE_TAU_LO, tau_hi = PLATE_TAU_HI, w_a = PLATE_W_A }
M.SIGMA_FLOOR, M.SIGMA_SCALE, M.EE_MIN = PLATE_SIGMA_FLOOR, PLATE_SIGMA_SCALE, PLATE_EE_MIN
M.CHECK_MAX, M.CHECK_ALPHA, M.RETAKE_CD = PLATE_CHECK_MAX, PLATE_CHECK_ALPHA, PLATE_RETAKE_CD
-- 셰이더 uniform 으로 주입하는 D9·D10 상수 {uniform 이름 = 값} (ac_plate_keyer.set_params 가 순회 없이 이름으로 읽는다)
M.SHADER_CONSTS = {
  same_enable = PLATE_SAME_ENABLE and 1.0 or 0.0, same_a_lo = PLATE_SAME_A_LO, same_a_hi = PLATE_SAME_A_HI,
  same_dc_max = PLATE_SAME_DC_MAX, same_dc_w = PLATE_SAME_DC_W, same_chroma_min = PLATE_SAME_CHROMA_MIN,
  same_gate_w = PLATE_SAME_GATE_W, chroma_sum_min = PLATE_CHROMA_SUM_MIN, chroma_gray = PLATE_CHROMA_GRAY,
  ring_px = PLATE_EDGE_RING_PX, edge_boundary_alpha = PLATE_EDGE_BOUNDARY_ALPHA, unmix_alpha_min = PLATE_UNMIX_ALPHA_MIN,
  unmix_denom_min = PLATE_UNMIX_DENOM_MIN, edge_tie_eps = PLATE_EDGE_TIE_EPS,
  unmix_clamp = PLATE_UNMIX_CLAMP_FHAT and 1.0 or 0.0, keep_alpha_max = PLATE_KEEP_ALPHA_MAX,
}

-- ---- float32 흉내: numpy 정답이 float32 로 계산하므로 같은 자리에서 반올림한다 ----
local F32 = ffi.new("float[1]")
local function f32(x) F32[0] = x; return F32[0] end

-- sRGB 바이트 -> 선형 (float32 LUT, 정답 srgb_to_linear 와 같은 식)
local DECODE_LUT = ffi.new("float[256]")
for v = 0, 255 do
  local x = f32(v / RGB_MAX)
  if x <= SRGB_DECODE_CUTOFF then
    DECODE_LUT[v] = f32(x / f32(12.92))
  else
    DECODE_LUT[v] = f32(f32(f32(x + f32(0.055)) / f32(1.055)) ^ f32(2.4))
  end
end

-- 선형 -> sRGB (정답 linear_to_srgb 와 같은 float32 단계)
local function encode(u)
  local x = f32(u)
  if x < 0 then x = 0 end
  if x <= SRGB_LINEAR_CUTOFF then return f32(f32(12.92) * x) end
  return f32(f32(f32(1.055) * f32(x ^ f32(1 / 2.4))) - f32(0.055))
end

-- ---- 누적: 프레임 N장의 선형 합과 제곱합 ----
-- 제곱합은 채널 합으로 줄여 픽셀당 1값만 둔다(σ 가 채널 평균 분산이라 결과가 같다). 메모리 = 픽셀당 double 4개
function M.new_accumulator(w, h)
  local n = w * h
  return { w = w, h = h, count = 0, sum = ffi.new("double[?]", n * RGB_STRIDE), sumsq = ffi.new("double[?]", n) }
end

function M.accumulate(acc, rgba)
  local sum, sumsq = acc.sum, acc.sumsq
  for p = 0, acc.w * acc.h - 1 do
    local i, o = p * RGBA_STRIDE, p * RGB_STRIDE
    local r, g, b = DECODE_LUT[rgba[i]], DECODE_LUT[rgba[i + 1]], DECODE_LUT[rgba[i + 2]]
    sum[o], sum[o + 1], sum[o + 2] = sum[o] + r, sum[o + 1] + g, sum[o + 2] + b
    sumsq[p] = sumsq[p] + r * r + g * g + b * b
  end
  acc.count = acc.count + 1
end

-- 누적 -> E(sRGB 0~1, float[3n]), σ(float[n], 하한 적용). 프레임이 없으면 nil
function M.plate_stats(acc)
  if acc.count == 0 then return nil end
  local n, count = acc.w * acc.h, acc.count
  local e, sigma = ffi.new("float[?]", n * RGB_STRIDE), ffi.new("float[?]", n)
  for p = 0, n - 1 do
    local o = p * RGB_STRIDE
    local m1, m2, m3 = acc.sum[o] / count, acc.sum[o + 1] / count, acc.sum[o + 2] / count
    local var = (acc.sumsq[p] / count - (m1 * m1 + m2 * m2 + m3 * m3)) / RGB_STRIDE
    sigma[p] = math.max(math.sqrt(math.max(var, 0)), PLATE_SIGMA_FLOOR)
    e[o], e[o + 1], e[o + 2] = encode(m1), encode(m2), encode(m3)
  end
  return e, sigma
end

local function to_byte(x)  -- 0~1 float32 -> 0~255 반올림(짝수 쪽)
  if x < 0 then x = 0 elseif x > 1 then x = 1 end
  return tune_math.round_half_even(f32(x * RGB_MAX))
end

-- (E, σ) -> RGBA8 버퍼(uint8_t[4n]). rgb = E, a = σ * SCALE
function M.pack_plate(e, sigma, w, h)
  local n = w * h
  local out = ffi.new("uint8_t[?]", n * RGBA_STRIDE)
  for p = 0, n - 1 do
    local i, o = p * RGBA_STRIDE, p * RGB_STRIDE
    out[i], out[i + 1], out[i + 2] = to_byte(e[o]), to_byte(e[o + 1]), to_byte(e[o + 2])
    out[i + 3] = to_byte(f32(sigma[p] * PLATE_SIGMA_SCALE))
  end
  return out
end

-- ---- CPU 판정 (셰이더와 같은 식) ----
local function to_linear(rgba, n)
  local out = ffi.new("float[?]", n * RGB_STRIDE)
  for p = 0, n - 1 do
    local i, o = p * RGBA_STRIDE, p * RGB_STRIDE
    out[o], out[o + 1], out[o + 2] = DECODE_LUT[rgba[i]], DECODE_LUT[rgba[i + 1]], DECODE_LUT[rgba[i + 2]]
  end
  return out
end

-- 판의 σ(알파 바이트 -> σ, 하한 이상)
local function plate_sigma(plate, n)
  local out = ffi.new("float[?]", n)
  for p = 0, n - 1 do
    out[p] = math.max(plate[p * RGBA_STRIDE + 3] / (RGB_MAX * PLATE_SIGMA_SCALE), PLATE_SIGMA_FLOOR)
  end
  return out
end

-- 색도 v/(r+g+b), 합이 거의 0이면 PLATE_CHROMA_SUM_MIN 으로 나눈다
local function chromaticity(v1, v2, v3)
  local d = math.max(v1 + v2 + v3, PLATE_CHROMA_SUM_MIN)
  return v1 / d, v2 / d, v3 / d
end

-- 한 샘플의 (CD, a, dc). 판이 거의 검정이면 a=1, CD = ‖I‖/σ. dc = 색도 거리 ‖c(I) - c(E)‖
local function distortion(i1, i2, i3, e1, e2, e3, sigma)
  local ee = e1 * e1 + e2 * e2 + e3 * e3
  local ci1, ci2, ci3 = chromaticity(i1, i2, i3)
  local ce1, ce2, ce3 = chromaticity(e1, e2, e3)
  local dc = math.sqrt((ci1 - ce1) ^ 2 + (ci2 - ce2) ^ 2 + (ci3 - ce3) ^ 2)
  if ee <= PLATE_EE_MIN then return math.sqrt(i1 * i1 + i2 * i2 + i3 * i3) / sigma, 1.0, dc end
  local a = (i1 * e1 + i2 * e2 + i3 * e3) / ee
  local r1, r2, r3 = i1 - a * e1, i2 - a * e2, i3 - a * e3
  return math.sqrt(r1 * r1 + r2 * r2 + r3 * r3) / sigma, a, dc
end

local function clamp_index(v, max) return v < 0 and 0 or (v > max and max or v) end

-- 가장자리 복제 쌍선형: 네 모서리 픽셀 번호 q 와 가중치 k 를 채운다
local function corners(x, y, w, h, off, q, k)
  local x0, y0 = math.floor(off[1]), math.floor(off[2])
  local fx, fy = off[1] - x0, off[2] - y0
  local xa, xb = clamp_index(x + x0, w - 1), clamp_index(x + x0 + 1, w - 1)
  local ya, yb = clamp_index(y + y0, h - 1) * w, clamp_index(y + y0 + 1, h - 1) * w
  q[1], q[2], q[3], q[4] = ya + xa, ya + xb, yb + xa, yb + xb
  k[1], k[2], k[3], k[4] = (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy
end

local function blend(arr, stride, c, q, k)
  return arr[q[1] * stride + c] * k[1] + arr[q[2] * stride + c] * k[2]
    + arr[q[3] * stride + c] * k[3] + arr[q[4] * stride + c] * k[4]
end

-- 9샘플 박스 평균 (CD, a, dc): float[n] 세 배열
function M.plate_distortions(img, plate, w, h)
  local n = w * h
  local i_lin, e_lin, sigma = to_linear(img, n), to_linear(plate, n), plate_sigma(plate, n)
  local cd_out, a_out, dc_out = ffi.new("float[?]", n), ffi.new("float[?]", n), ffi.new("float[?]", n)
  local q, k = {}, {}
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      local cd_sum, a_sum, dc_sum = 0.0, 0.0, 0.0
      for _, s in ipairs(BOX_SAMPLES) do
        corners(x, y, w, h, s, q, k)
        local cd, a, dc = distortion(blend(i_lin, RGB_STRIDE, 0, q, k), blend(i_lin, RGB_STRIDE, 1, q, k),
          blend(i_lin, RGB_STRIDE, 2, q, k), blend(e_lin, RGB_STRIDE, 0, q, k), blend(e_lin, RGB_STRIDE, 1, q, k),
          blend(e_lin, RGB_STRIDE, 2, q, k), blend(sigma, 1, 0, q, k))
        cd_sum, a_sum, dc_sum = cd_sum + s[3] * cd, a_sum + s[3] * a, dc_sum + s[3] * dc
      end
      local o = y * w + x
      cd_out[o], a_out[o], dc_out[o] = cd_sum / BOX_WEIGHT_SUM, a_sum / BOX_WEIGHT_SUM, dc_sum / BOX_WEIGHT_SUM
    end
  end
  return cd_out, a_out, dc_out
end

local function saturate(v) return v < 0 and 0 or (v > 1 and 1 or v) end

-- 판 픽셀별 색도 채도 ‖c(E) - 1/3‖ (선형 E). D9 무채색 gate 입력 (정답 plate_chroma)
local function plate_chroma(plate, w, h)
  local n = w * h
  local out = ffi.new("float[?]", n)
  for p = 0, n - 1 do
    local i = p * RGBA_STRIDE
    local c1, c2, c3 = chromaticity(DECODE_LUT[plate[i]], DECODE_LUT[plate[i + 1]], DECODE_LUT[plate[i + 2]])
    out[p] = math.sqrt((c1 - PLATE_CHROMA_GRAY) ^ 2 + (c2 - PLATE_CHROMA_GRAY) ^ 2 + (c3 - PLATE_CHROMA_GRAY) ^ 2)
  end
  return out
end

-- D9 same = sat((DC_MAX+DC_W-dc)/DC_W) · sat((a-A_LO)/W_A) · sat((A_HI+W_A-a)/W_A) · sat((chroma-CHROMA_MIN)/GATE_W)
local function same_factor(a, dc, chroma, p, w_a)
  local a_lo, a_hi = p.same_a_lo or PLATE_SAME_A_LO, p.same_a_hi or PLATE_SAME_A_HI
  local dc_max, dc_w = p.same_dc_max or PLATE_SAME_DC_MAX, p.same_dc_w or PLATE_SAME_DC_W
  local chroma_min = p.same_chroma_min or PLATE_SAME_CHROMA_MIN
  return saturate((dc_max + dc_w - dc) / dc_w) * saturate((a - a_lo) / w_a) * saturate((a_hi + w_a - a) / w_a)
    * saturate((chroma - chroma_min) / PLATE_SAME_GATE_W)
end

-- 전경 알파(float[n]) + 평균 CD + 픽셀별 CD(float[n]).
-- alpha = max(sat((CD-τcd)/wcd), sat((τlo-a)/wa), sat((a-τhi)/wa)) · (1 - same)  (same 은 D9 켜졌을 때만)
function M.plate_key(img, plate, w, h, params)
  local p = params or {}
  local tau_cd, w_cd = p.tau_cd or PLATE_TAU_CD, p.w_cd or PLATE_W_CD
  local tau_lo, tau_hi, w_a = p.tau_lo or PLATE_TAU_LO, p.tau_hi or PLATE_TAU_HI, p.w_a or PLATE_W_A
  local same_on = PLATE_SAME_ENABLE
  if p.same_enable ~= nil then same_on = p.same_enable end
  local cd, a, dc = M.plate_distortions(img, plate, w, h)
  local chroma = same_on and plate_chroma(plate, w, h) or nil
  local n = w * h
  local alpha, cd_total = ffi.new("float[?]", n), 0.0
  for i = 0, n - 1 do
    local v = math.max(saturate((cd[i] - tau_cd) / w_cd), saturate((tau_lo - a[i]) / w_a), saturate((a[i] - tau_hi) / w_a))
    if same_on then v = v * (1 - same_factor(a[i], dc[i], chroma[i], p, w_a)) end
    alpha[i] = v
    cd_total = cd_total + cd[i]
  end
  return alpha, cd_total / n, cd
end

-- 사람 없는 화면 점검. 반환 ok(전경 비율 <= CHECK_MAX), 전경 비율, 평균 CD, 다시 찍기 권장(bool)
function M.plate_check(img, plate, w, h, params)
  local alpha, mean_cd = M.plate_key(img, plate, w, h, params)
  local n, fg = w * h, 0
  for i = 0, n - 1 do
    if alpha[i] > PLATE_CHECK_ALPHA then fg = fg + 1 end
  end
  local ratio = fg / n
  return ratio <= PLATE_CHECK_MAX, ratio, mean_cd, PLATE_RETAKE_CD > 0 and mean_cd > PLATE_RETAKE_CD
end

return M
