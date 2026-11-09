-- ac_autoplate.lua — 자동 배경판 계산 (PLAN-v0.3 D15 ③⑤⑥⑧⑩⑪, D23). OBS 의존 없음.
-- 사람이 서 있는 화면 한 장에서 배경지 모양을 추정해 배경판 RGBA8 을 만든다. 정답 원본: sim/autoplate.py (float64).
-- 판 바이트 규칙: 지울 수 있는 곳(배경지 칸에서 SUPPORT_PX 이내 ∧ 분석 영역) 밖은 알파 KEEP_BYTE(0), 안은 σ 바이트.
-- 수정 시 tests/lua/run_parity.sh 필수(D24: 배경지 칸·지울 곳 불일치 <= 0.2%, 바이트 |차|<=1 픽셀 >= 99.5%).
-- 단방향 의존: ac_autoplate -> ac_autoplate_img(기본 영상 연산), ac_roi(분석 제외), ac_plate_math(σ 하한·반올림).
local ffi = require("ffi")
local img = require("ac_autoplate_img")
local roi_mod = require("ac_roi")
local plate_math = require("ac_plate_math")
local tune_math = require("ac_tune_math")

local M = {}
local floor, ceil, min, max, sqrt, abs = math.floor, math.ceil, math.min, math.max, math.sqrt, math.abs

-- ---- 상수: sim/constants_autoplate.py 와 같은 값 (한 줄 한 선언, 정합 시험 [34] 대상. 단일 출처 = 이 파일) ----
local AUTOPLATE_DOWN = 8              -- 저해상도 배율(칸 = 8x8 px)
local SEED_TOP_SHARE = 0.35           -- 씨앗 색을 찾는 화면 위쪽 비율
local AB_BIN = 2.0                    -- (a,b) 히스토그램 칸 폭
local SEED_AB_TOL = 8.0               -- 씨앗과의 (a,b) 거리 허용
local SEED_L_TOL = 22.0               -- 씨앗과의 밝기 차 허용
local TEXTURE_MAX = 3.0               -- 3x3 밝기 표준편차 상한(매끈한 칸)
local GROW_DE = 3.0                   -- 넓히기: 이웃 배경지로 추정한 색과의 ΔE 허용
local GROW_ITERS = 40                 -- 넓히기 반복(1회 = 저해상도 1칸)
local MIN_BLOB_SHARE = 0.02           -- 이보다 작은 덩어리는 버림(칸 수 대비)
local SIGMA_SAFETY = 1.5              -- σ 여유 배수
local SUPPORT_PX = 48                 -- 배경지 칸에서 이 거리(원해상도 px) 밖은 지우지 않는다
local NOISE_FRAMES = 5                -- 시간 잡음 계산 프레임 수
local KEEP_BYTE = 0                   -- 판 알파 바이트 0 = 지우지 않음 (D16)
-- 구현 내부 상수
local KEY_OFFSET = 1024               -- (a,b) 칸 번호를 음수 없이 한 정수로 묶기 위한 오프셋
local KEY_SPAN = 4096                 -- 위 묶음의 b 자리 폭
local RGBA_BYTES = 4
local RGB_MAX = 255.0
local MSG_TOO_SMALL = "분석 영역이 너무 작습니다"
local MSG_FRAME_SIZE = "프레임 크기가 기준 화면과 다릅니다"
local NO_ROI = { 0, 0, 0, 0 }

M.AUTOPLATE_DOWN, M.SEED_TOP_SHARE, M.AB_BIN, M.SEED_AB_TOL, M.SEED_L_TOL = AUTOPLATE_DOWN, SEED_TOP_SHARE, AB_BIN, SEED_AB_TOL, SEED_L_TOL
M.TEXTURE_MAX, M.GROW_DE, M.GROW_ITERS, M.MIN_BLOB_SHARE = TEXTURE_MAX, GROW_DE, GROW_ITERS, MIN_BLOB_SHARE
M.SIGMA_SAFETY, M.SUPPORT_PX, M.NOISE_FRAMES, M.KEEP_BYTE = SIGMA_SAFETY, SUPPORT_PX, NOISE_FRAMES, KEEP_BYTE

-- 분석 영역: px = {y0,y1,x0,x1}(원해상도), cells = {r0,r1,c0,c1}(영역 안에 완전히 든 칸만). 실패면 nil, 사유
local function roi_rects(h, w, roi)
  local reason = roi_mod.check(w, h, roi)
  if reason then return nil, nil, reason end
  local y0, y1, x0, x1 = roi[1], h - roi[2], roi[3], w - roi[4]
  local rows, cols = floor(h / AUTOPLATE_DOWN), floor(w / AUTOPLATE_DOWN)
  local c = { ceil(y0 / AUTOPLATE_DOWN), min(floor(y1 / AUTOPLATE_DOWN), rows), ceil(x0 / AUTOPLATE_DOWN), min(floor(x1 / AUTOPLATE_DOWN), cols) }
  if c[1] >= c[2] or c[3] >= c[4] then return nil, nil, MSG_TOO_SMALL end
  return { y0, y1, x0, x1 }, c, nil
end
M.roi_rects = roi_rects

local function rect_mask(rows, cols, c)
  local out = ffi.new("uint8_t[?]", rows * cols)
  for y = c[1], c[2] - 1 do for x = c[3], c[4] - 1 do out[y * cols + x] = 1 end end
  return out
end

local function count_set(mask, n)
  local s = 0
  for i = 0, n - 1 do if mask[i] ~= 0 then s = s + 1 end end
  return s
end

-- 분석 영역이 걸친 첫 행과 행 수
local function analysis_span(analysis, rows, cols)
  local first, last
  for y = 0, rows - 1 do
    for x = 0, cols - 1 do
      if analysis[y * cols + x] ~= 0 then first, last = first or y, y; break end
    end
  end
  return first, last - first + 1
end

local function median(values)
  table.sort(values)
  local n = #values
  if n % 2 == 1 then return values[(n + 1) / 2] end
  return (values[n / 2] + values[n / 2 + 1]) / 2
end

-- ③ 씨앗 색: 분석 영역 위쪽 SEED_TOP_SHARE 행의 (a,b) 칸 히스토그램 최빈값(동점은 사전순 첫째) 칸들의 Lab 중앙값
local function seed_color(lab, analysis, rows, cols)
  local r0, span = analysis_span(analysis, rows, cols)
  local r_end = min(r0 + max(floor(span * SEED_TOP_SHARE), 1), rows)
  local counts, keys = {}, {}
  for y = r0, r_end - 1 do
    for x = 0, cols - 1 do
      local i = y * cols + x
      if analysis[i] ~= 0 then
        local key = (floor(lab.d[i * 3 + 1] / AB_BIN) + KEY_OFFSET) * KEY_SPAN + floor(lab.d[i * 3 + 2] / AB_BIN) + KEY_OFFSET
        keys[i] = key
        counts[key] = (counts[key] or 0) + 1
      end
    end
  end
  local best_key, best_n = nil, -1
  for key, n in pairs(counts) do
    if n > best_n or (n == best_n and key < best_key) then best_key, best_n = key, n end
  end
  local l, a, b = {}, {}, {}
  for i, key in pairs(keys) do
    if key == best_key then l[#l + 1], a[#a + 1], b[#b + 1] = lab.d[i * 3], lab.d[i * 3 + 1], lab.d[i * 3 + 2] end
  end
  return { median(l), median(a), median(b) }
end

-- 면적 >= MIN_BLOB_SHARE(분석 칸 대비) ∧ 윗변이 분석 영역 위쪽 SEED_TOP_SHARE 안인 덩어리, 없으면 가장 큰 것(동점은 번호 작은 것)
local function pick_blobs(cand, analysis, rows, cols)
  local labels, count, areas, tops = img.label8(cand, rows, cols)
  if count == 0 then return cand end
  local r0, span = analysis_span(analysis, rows, cols)
  local top_limit, area_min = r0 + span * SEED_TOP_SHARE, MIN_BLOB_SHARE * count_set(analysis, rows * cols)
  local keep, any = {}, false
  for id = 1, count do
    if areas[id] >= area_min and tops[id] <= top_limit then keep[id], any = true, true end
  end
  if not any then
    local best = 1
    for id = 2, count do if areas[id] > areas[best] then best = id end end
    keep[best] = true
  end
  local out = ffi.new("uint8_t[?]", rows * cols)
  for i = 0, rows * cols - 1 do if labels[i] ~= 0 and keep[labels[i]] then out[i] = 1 end end
  return out
end

-- 배경지 후보 덩어리 칸: 씨앗 색 ∧ 매끈함 ∧ 분석 영역 (④⑤). 반환: mask, seed
local function paper_mask(lab, analysis, rows, cols)
  local n = rows * cols
  local seed = seed_color(lab, analysis, rows, cols)
  local light, light_sq = ffi.new("double[?]", n), ffi.new("double[?]", n)
  for i = 0, n - 1 do light[i] = lab.d[i * 3]; light_sq[i] = light[i] * light[i] end
  local mean_sq, mean = img.blur3(light_sq, rows, cols), img.blur3(light, rows, cols)
  local cand = ffi.new("uint8_t[?]", n)
  for i = 0, n - 1 do
    local var = mean_sq[i] - mean[i] * mean[i]
    local da, db = lab.d[i * 3 + 1] - seed[2], lab.d[i * 3 + 2] - seed[3]
    if analysis[i] ~= 0 and sqrt(max(var, 0.0)) < TEXTURE_MAX and sqrt(da * da + db * db) < SEED_AB_TOL
      and abs(light[i] - seed[1]) < SEED_L_TOL then cand[i] = 1 end
  end
  return pick_blobs(cand, analysis, rows, cols), seed
end

-- 마스크(uint8) -> 가중치 1채널 이미지
local function mask_weights(mask, rows, cols)
  local wt = img.new_img(rows, cols, 1)
  for i = 0, rows * cols - 1 do wt.d[i] = mask[i] end
  return wt
end

-- ⑥ 넓히기: 둘레 칸 중 push-pull 모델 색과 ΔE < GROW_DE 인 칸을 최대 GROW_ITERS 회 더한다. 반환: 새 마스크, 실제 반복 수
local function grow(small, lab, mask, analysis, rows, cols)
  local n, done = rows * cols, 0
  local cur = ffi.new("uint8_t[?]", n)
  ffi.copy(cur, mask, n)
  for _ = 1, GROW_ITERS do
    local near = img.dilate(cur, rows, cols, img.BOX3_OFFSETS)
    local model, add = nil, {}
    for i = 0, n - 1 do
      if near[i] ~= 0 and cur[i] == 0 and analysis[i] ~= 0 then
        model = model or img.push_pull(small, mask_weights(cur, rows, cols))
        local l, a, b = img.lab_of(model.d[i * 3], model.d[i * 3 + 1], model.d[i * 3 + 2])
        local dl, da, db = lab.d[i * 3] - l, lab.d[i * 3 + 1] - a, lab.d[i * 3 + 2] - b
        if sqrt(dl * dl + da * da + db * db) < GROW_DE then add[#add + 1] = i end
      end
    end
    if #add == 0 then break end
    for _, i in ipairs(add) do cur[i] = 1 end
    done = done + 1
  end
  return cur, done
end

-- ⑩ 시간 잡음: 누적(acc)의 픽셀별 시간 표준편차(선형, 채널 RMS)를 칸 평균한 값의 배경지 칸 중앙값. 2장 미만이면 0
local function temporal_noise(acc, mask, rows, cols)
  if not acc or acc.count < 2 then return 0.0 end
  local cells = ffi.new("double[?]", rows * cols)
  for y = 0, rows * AUTOPLATE_DOWN - 1 do
    local base = floor(y / AUTOPLATE_DOWN) * cols
    for x = 0, cols * AUTOPLATE_DOWN - 1 do
      local p = y * acc.w + x
      local m1, m2, m3 = acc.sum[p * 3] / acc.count, acc.sum[p * 3 + 1] / acc.count, acc.sum[p * 3 + 2] / acc.count
      local var = (acc.sumsq[p] / acc.count - (m1 * m1 + m2 * m2 + m3 * m3)) / 3
      local c = base + floor(x / AUTOPLATE_DOWN)
      cells[c] = cells[c] + sqrt(max(var, 0.0))
    end
  end
  local use_mask = count_set(mask, rows * cols) > 0
  local picked = {}
  for i = 0, rows * cols - 1 do
    if not use_mask or mask[i] ~= 0 then picked[#picked + 1] = cells[i] / (AUTOPLATE_DOWN * AUTOPLATE_DOWN) end
  end
  return median(picked)
end

-- σ = max(하한, SIGMA_SAFETY × 잡음) -> 판 알파 바이트(σ × SCALE × 255, 반올림 짝수 쪽)
local function sigma_byte(noise)
  local sigma = max(plate_math.SIGMA_FLOOR, SIGMA_SAFETY * noise)
  return tune_math.round_half_even(min(sigma * plate_math.SIGMA_SCALE, 1.0) * RGB_MAX)
end
M.sigma_byte = sigma_byte

-- ⑧⑪ 판 알파: 지울 수 있는 곳(칸 단위) -> 원해상도(가장 가까운 칸) ∧ 분석 영역 사각형 = σ 바이트, 그 밖 KEEP_BYTE
local function fill_alpha(plate, support, px, w, rows, cols, sbyte)
  local cx = ffi.new("int32_t[?]", w)
  for x = 0, w - 1 do cx[x] = min(floor(x / AUTOPLATE_DOWN), cols - 1) end
  for y = px[1], px[2] - 1 do
    local row = min(floor(y / AUTOPLATE_DOWN), rows - 1) * cols
    for x = px[3], px[4] - 1 do
      plate[(y * w + x) * RGBA_BYTES + 3] = support[row + cx[x]] ~= 0 and sbyte or KEEP_BYTE
    end
  end
end

-- (rgba uint8 W*H*4, 폭, 높이, roi={위,아래,왼,오}|nil, acc=ac_plate_math 누적기|nil) -> 판 uint8[W*H*4], info / 실패면 nil, 사유
function M.compute(rgba, w, h, roi, acc)
  local px, cells, reason = roi_rects(h, w, roi or NO_ROI)
  if not px then return nil, reason end
  if acc and (acc.w ~= w or acc.h ~= h) then return nil, MSG_FRAME_SIZE end
  local small = img.block_mean_lin(rgba, w, h, AUTOPLATE_DOWN)
  local rows, cols = small.h, small.w
  local lab = img.lab_plane(small)
  local analysis = rect_mask(rows, cols, cells)
  local mask0, seed = paper_mask(lab, analysis, rows, cols)
  local mask, grown = grow(small, lab, mask0, analysis, rows, cols)
  local model = img.push_pull(small, mask_weights(mask, rows, cols))
  local support = img.dilate(mask, rows, cols, img.disk_offsets(floor(SUPPORT_PX / AUTOPLATE_DOWN)))
  for i = 0, rows * cols - 1 do if analysis[i] == 0 then support[i] = 0 end end
  local noise = temporal_noise(acc, mask, rows, cols)
  local sbyte = sigma_byte(noise)
  local plate = ffi.new("uint8_t[?]", w * h * RGBA_BYTES)
  img.model_to_rgba(model, h, w, AUTOPLATE_DOWN, plate)
  fill_alpha(plate, support, px, w, rows, cols, sbyte)
  return plate, {
    paper_share = count_set(mask, rows * cols) / (rows * cols), sigma_byte = sbyte, noise = noise, grow_iters = grown,
    seed_lab = seed, paper_mask = mask, mask0 = mask0, support = support, small_lin = small, lab = lab, rows = rows, cols = cols,
  }
end

return M
