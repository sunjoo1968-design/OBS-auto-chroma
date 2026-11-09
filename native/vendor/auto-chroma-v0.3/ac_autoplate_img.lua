-- ac_autoplate_img.lua — 자동 배경판의 기본 영상 연산 (PLAN-v0.3 D15 ①②④⑦⑧⑨ + 8-연결 덩어리). OBS 의존 없음.
-- 정답 원본: sim/autoplate_img.py (numpy float64). 여기는 FFI double 배열로 같은 연산·같은 합 순서를 따른다.
-- 수정 시 tests/lua/run_parity.sh 필수(칸 선형·Lab·push-pull 모델 |차| <= 1e-9, 마스크·바이트 패리티).
-- 이미지 표현: { h = 행 수, w = 열 수, c = 채널 수, d = double[h*w*c] (행 우선, 채널 끼움) }. 인덱스는 0부터.
-- ac_autoplate(알고리즘 본체)가 이 모듈을 require 한다(단방향). AUTOPLATE_* 상수는 ac_autoplate 가 단일 출처.
local ffi = require("ffi")

local M = {}
local floor, ceil, min, max, sqrt = math.floor, math.ceil, math.min, math.max, math.sqrt

-- ---- 상수: sim/constants.py(sRGB·Lab) · sim/constants_autoplate.py(push-pull·평활) 와 같은 값, 한 줄 한 선언 ----
local RGB_MAX = 255.0
local SRGB_DECODE_CUTOFF = 0.04045
local SRGB_LINEAR_CUTOFF = 0.0031308
local SRGB_A = 0.055
local SRGB_GAMMA = 2.4
local SRGB_LINEAR_SLOPE = 12.92
local LAB_EPSILON = 216.0 / 24389.0
local LAB_CBRT_EXP = 1.0 / 3.0
local LAB_LINEAR_OFFSET = 4.0 / 29.0
local LAB_LINEAR_SLOPE = 1.0 / (3.0 * (6.0 / 29.0) ^ 2)
local LAB_L_SCALE, LAB_L_OFFSET, LAB_A_SCALE, LAB_B_SCALE = 116.0, 16.0, 500.0, 200.0
local SMOOTH_K0, SMOOTH_K1, SMOOTH_K2 = 0.25, 0.5, 0.25
local PUSHPULL_MIN_SIDE = 2
local PUSHPULL_WEIGHT_GAIN = 4.0
local EPS_WEIGHT = 1e-6
local D65_X, D65_Y, D65_Z = 0.95047, 1.0, 1.08883
local SRGB_TO_XYZ = {
  { 0.4124564, 0.3575761, 0.1804375 },
  { 0.2126729, 0.7151522, 0.0721750 },
  { 0.0193339, 0.1191920, 0.9503041 },
}
local WHITE = { D65_X, D65_Y, D65_Z }

-- 행 합 = 기준 백색이 되도록 행 단위로 맞춘 행렬(파이썬 _RGB_TO_XYZ 와 같은 식·같은 합 순서)
local XYZ = {}
for r = 1, 3 do
  local row = SRGB_TO_XYZ[r]
  local factor = WHITE[r] / ((row[1] + row[2]) + row[3])
  XYZ[r] = { row[1] * factor, row[2] * factor, row[3] * factor }
end
local X0, X1, X2, Y0, Y1, Y2, Z0, Z1, Z2 = XYZ[1][1], XYZ[1][2], XYZ[1][3], XYZ[2][1], XYZ[2][2], XYZ[2][3], XYZ[3][1], XYZ[3][2], XYZ[3][3]

M.BOX3_OFFSETS = {}
for dy = -1, 1 do for dx = -1, 1 do M.BOX3_OFFSETS[#M.BOX3_OFFSETS + 1] = { dy, dx } end end

function M.new_img(h, w, c) return { h = h, w = w, c = c, d = ffi.new("double[?]", h * w * c) } end

-- ---- sRGB 부호화·복호화 (정답 srgb_encode / srgb_decode, 스칼라) ----
local function decode(x)
  if x <= SRGB_DECODE_CUTOFF then return x / SRGB_LINEAR_SLOPE end
  return ((max(x, 0.0) + SRGB_A) / (1.0 + SRGB_A)) ^ SRGB_GAMMA
end
local function encode(u)
  local x = max(u, 0.0)
  if x <= SRGB_LINEAR_CUTOFF then return SRGB_LINEAR_SLOPE * x end
  return (1.0 + SRGB_A) * x ^ (1.0 / SRGB_GAMMA) - SRGB_A
end
M.decode, M.encode = decode, encode

local DECODE_LUT = ffi.new("double[256]")
for v = 0, 255 do DECODE_LUT[v] = decode(v / RGB_MAX) end

local function clamp01(x) if x < 0.0 then return 0.0 elseif x > 1.0 then return 1.0 end return x end

-- ① 축소: down x down 블록의 선형 평균. 나머지 행·열은 버린다. rgba = uint8 RGBA (W*H*4)
function M.block_mean_lin(rgba, w_px, h_px, down)
  local rows, cols = floor(h_px / down), floor(w_px / down)
  assert(rows > 0 and cols > 0, "이미지가 블록 크기보다 작습니다")
  local out = M.new_img(rows, cols, 3)
  local d, area = out.d, down * down
  for y = 0, rows * down - 1 do
    local base = floor(y / down) * cols
    for x = 0, cols * down - 1 do
      local i, o = (y * w_px + x) * 4, (base + floor(x / down)) * 3
      d[o] = d[o] + DECODE_LUT[rgba[i]]
      d[o + 1] = d[o + 1] + DECODE_LUT[rgba[i + 1]]
      d[o + 2] = d[o + 2] + DECODE_LUT[rgba[i + 2]]
    end
  end
  for i = 0, rows * cols * 3 - 1 do d[i] = d[i] / area end
  return out
end

-- ② 선형 RGB 한 칸 -> Lab (정답 srgb_to_lab(srgb_encode(·)): 부호화 -> 0~1 로 자름 -> 복호화 -> XYZ -> Lab)
local function lab_f(t)
  if t > LAB_EPSILON then return t ^ LAB_CBRT_EXP end
  return t * LAB_LINEAR_SLOPE + LAB_LINEAR_OFFSET
end
function M.lab_of(r, g, b)
  r, g, b = decode(clamp01(encode(r))), decode(clamp01(encode(g))), decode(clamp01(encode(b)))
  local fx = lab_f((X0 * r + X1 * g + X2 * b) / D65_X)
  local fy = lab_f((Y0 * r + Y1 * g + Y2 * b) / D65_Y)
  local fz = lab_f((Z0 * r + Z1 * g + Z2 * b) / D65_Z)
  return LAB_L_SCALE * fy - LAB_L_OFFSET, LAB_A_SCALE * (fx - fy), LAB_B_SCALE * (fy - fz)
end

function M.lab_plane(lin)
  local out = M.new_img(lin.h, lin.w, 3)
  for i = 0, lin.h * lin.w - 1 do
    out.d[i * 3], out.d[i * 3 + 1], out.d[i * 3 + 2] = M.lab_of(lin.d[i * 3], lin.d[i * 3 + 1], lin.d[i * 3 + 2])
  end
  return out
end

-- ④ 3x3 가우시안: 축 0 -> 축 1 순서, 경계는 가장자리 칸 제외 반사(REFLECT_101). 길이 1 축은 건너뜀. 1채널 평면
local function reflect(i, n)
  if i < 0 then return -i end
  if i >= n then return 2 * n - 2 - i end
  return i
end
local function blur_axis(src, h, w, axis)
  local n = axis == 0 and h or w
  if n < 2 then return src end
  local out = ffi.new("double[?]", h * w)
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      local lo, hi
      if axis == 0 then lo, hi = reflect(y - 1, h) * w + x, reflect(y + 1, h) * w + x
      else lo, hi = y * w + reflect(x - 1, w), y * w + reflect(x + 1, w) end
      out[y * w + x] = SMOOTH_K0 * src[lo] + SMOOTH_K1 * src[y * w + x] + SMOOTH_K2 * src[hi]
    end
  end
  return out
end
function M.blur3(src, h, w) return blur_axis(blur_axis(src, h, w, 0), h, w, 1) end
-- ⑦ 2x2 블록 평균, 크기 (ceil(h/2), ceil(w/2)) — 마지막 홀수 행·열은 있는 칸만. 축 0 -> 축 1 순서
local function halve_axis(img, axis)
  local h, w, c = img.h, img.w, img.c
  local nh, nw = h, w
  if axis == 0 then nh = ceil(h / 2) else nw = ceil(w / 2) end
  local out = M.new_img(nh, nw, c)
  for y = 0, nh - 1 do
    for x = 0, nw - 1 do
      local sy, sx = y, x
      if axis == 0 then sy = 2 * y else sx = 2 * x end
      local py, px = sy, sx
      if axis == 0 then py = sy + 1 else px = sx + 1 end
      local has2 = py < h and px < w
      for k = 0, c - 1 do
        local a = img.d[(sy * w + sx) * c + k]
        out.d[(y * nw + x) * c + k] = has2 and (a + img.d[(py * w + px) * c + k]) / 2 or a
      end
    end
  end
  return out
end
local function halve(img) return halve_axis(halve_axis(img, 0), 1) end
-- 반 픽셀 중심 쌍선형의 축별 표본: 칸 i0, i1 와 가중 frac (정답 bilinear_resize)
local function axis_taps(n_in, n_out, s)
  local i0, i1, frac = ffi.new("int32_t[?]", n_out), ffi.new("int32_t[?]", n_out), ffi.new("double[?]", n_out)
  for i = 0, n_out - 1 do
    local pos = min(max((i + 0.5) * s - 0.5, 0.0), n_in - 1.0)
    local a = min(floor(pos), max(n_in - 2, 0))
    i0[i], i1[i], frac[i] = a, min(a + 1, n_in - 1), pos - a
  end
  return i0, i1, frac
end

local function bilinear_resize(img, out_h, out_w)
  local c = img.c
  local y0, y1, fy = axis_taps(img.h, out_h, img.h / out_h)
  local x0, x1, fx = axis_taps(img.w, out_w, img.w / out_w)
  local mid = M.new_img(out_h, img.w, c)
  for y = 0, out_h - 1 do
    for k = 0, img.w * c - 1 do
      mid.d[y * img.w * c + k] = img.d[y0[y] * img.w * c + k] * (1.0 - fy[y]) + img.d[y1[y] * img.w * c + k] * fy[y]
    end
  end
  local out = M.new_img(out_h, out_w, c)
  for y = 0, out_h - 1 do
    for x = 0, out_w - 1 do
      for k = 0, c - 1 do
        out.d[(y * out_w + x) * c + k] = mid.d[(y * img.w + x0[x]) * c + k] * (1.0 - fx[x]) + mid.d[(y * img.w + x1[x]) * c + k] * fx[x]
      end
    end
  end
  return out
end
-- 가중 평균 상수로 끝내는 바닥 경우
local function weighted_constant(img, wt, wsum)
  local n, c = img.h * img.w, img.c
  local out = M.new_img(img.h, img.w, c)
  for k = 0, c - 1 do
    local s = 0.0
    for i = 0, n - 1 do s = s + img.d[i * c + k] * wt.d[i] end
    local mean = s / max(wsum, EPS_WEIGHT)
    for i = 0, n - 1 do out.d[i * c + k] = mean end
  end
  return out
end

-- ⑦ push-pull: 가중치 wt(1채널, 0~1)가 있는 칸만으로 빈 칸을 매끈하게 채운다
local function push_pull(img, wt)
  local n, c = img.h * img.w, img.c
  local wsum = 0.0
  for i = 0, n - 1 do wsum = wsum + wt.d[i] end
  if min(img.h, img.w) <= PUSHPULL_MIN_SIDE or wsum == 0 then return weighted_constant(img, wt, wsum) end
  local weighted = M.new_img(img.h, img.w, c)
  for i = 0, n - 1 do for k = 0, c - 1 do weighted.d[i * c + k] = img.d[i * c + k] * wt.d[i] end end
  local si, sw = halve(weighted), halve(wt)
  local cw = M.new_img(sw.h, sw.w, 1)
  for i = 0, sw.h * sw.w - 1 do
    local denom = max(sw.d[i], EPS_WEIGHT)
    for k = 0, c - 1 do si.d[i * c + k] = si.d[i * c + k] / denom end
    cw.d[i] = min(sw.d[i] * PUSHPULL_WEIGHT_GAIN, 1.0)
  end
  local up = bilinear_resize(push_pull(si, cw), img.h, img.w)
  for i = 0, n - 1 do
    for k = 0, c - 1 do up.d[i * c + k] = weighted.d[i * c + k] + up.d[i * c + k] * (1.0 - wt.d[i]) end
  end
  return up
end
M.push_pull = push_pull

-- ⑨ 칸 모델(선형, rows x cols x 3) -> 원해상도 sRGB 바이트: 칸에서 먼저 부호화 -> 쌍선형 -> 반올림(짝수 쪽).
-- out = uint8 RGBA 버퍼(w_px*h_px*4)의 RGB 채널만 채운다(알파는 건드리지 않음)
function M.model_to_rgba(model, h_px, w_px, down, out)
  local cells, cols = model.h * model.w, model.w
  local enc = ffi.new("double[?]", cells * 3)
  for i = 0, cells * 3 - 1 do enc[i] = clamp01(encode(model.d[i])) end
  local scale = 1.0 / down
  local y0, y1, fy = axis_taps(model.h, h_px, scale)
  local x0, x1, fx = axis_taps(model.w, w_px, scale)
  local row = ffi.new("double[?]", cols * 3)
  for y = 0, h_px - 1 do
    local a, b, ty = y0[y] * cols * 3, y1[y] * cols * 3, fy[y]
    for k = 0, cols * 3 - 1 do row[k] = enc[a + k] * (1.0 - ty) + enc[b + k] * ty end
    for x = 0, w_px - 1 do
      local p, q, tx, o = x0[x] * 3, x1[x] * 3, fx[x], (y * w_px + x) * 4
      for k = 0, 2 do
        local v = clamp01(row[p + k] * (1.0 - tx) + row[q + k] * tx) * RGB_MAX
        local f = floor(v)
        local frac = v - f
        if frac > 0.5 or (frac == 0.5 and f % 2 == 1) then f = f + 1 end
        out[o + k] = f
      end
    end
  end
end

-- 8-연결 덩어리 번호(1부터, 배경 0). 번호 = 각 덩어리의 첫 칸이 행 우선으로 나오는 순서.
-- 반환: labels(int32[h*w]), count, areas(1..count), tops(1..count: 윗변 행)
function M.label8(mask, h, w)
  local labels, queue = ffi.new("int32_t[?]", h * w), ffi.new("int32_t[?]", h * w)
  local count, areas, tops = 0, {}, {}
  for s = 0, h * w - 1 do
    if mask[s] ~= 0 and labels[s] == 0 then
      count = count + 1
      labels[s], queue[0] = count, s
      local head, tail, top = 0, 1, floor(s / w)
      while head < tail do
        local cur = queue[head]
        head = head + 1
        local cy, cx = floor(cur / w), cur % w
        for dy = -1, 1 do
          local ny = cy + dy
          for dx = -1, 1 do
            local nx = cx + dx
            local nb = ny * w + nx
            if ny >= 0 and ny < h and nx >= 0 and nx < w and mask[nb] ~= 0 and labels[nb] == 0 then
              labels[nb], queue[tail] = count, nb
              tail = tail + 1
              if ny < top then top = ny end
            end
          end
        end
      end
      areas[count], tops[count] = tail, top
    end
  end
  return labels, count, areas, tops
end

-- ⑧ 반지름 radius 원판 오프셋(dy*dy+dx*dx <= r*r, 행 우선). 반지름 6 이면 113개
function M.disk_offsets(radius)
  local out = {}
  for dy = -radius, radius do
    for dx = -radius, radius do
      if dy * dy + dx * dx <= radius * radius then out[#out + 1] = { dy, dx } end
    end
  end
  return out
end

-- 오프셋 집합으로 팽창(경계 밖은 0). mask = uint8[h*w]
function M.dilate(mask, h, w, offsets)
  local out = ffi.new("uint8_t[?]", h * w)
  for _, off in ipairs(offsets) do
    local dy, dx = off[1], off[2]
    for y = max(dy, 0), h + min(dy, 0) - 1 do
      for x = max(dx, 0), w + min(dx, 0) - 1 do
        if mask[(y - dy) * w + (x - dx)] ~= 0 then out[y * w + x] = 1 end
      end
    end
  end
  return out
end

return M
