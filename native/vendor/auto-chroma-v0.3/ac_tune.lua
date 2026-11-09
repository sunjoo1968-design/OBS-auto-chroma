-- ac_tune.lua — 자동 맞춤 (sim/autotune.py + sim/hybrid_key.py 의 Lua 이식, OBS 의존 없는 순수 Lua)
-- 정답 원본은 파이썬이다. 수정 시 tests/lua/run_parity.sh 를 반드시 통과시킬 것.
-- 알고리즘(PLAN [10] ①~⑤): 키색=스크린 색(녹색·파랑 우세) 픽셀의 CbCr 최빈 봉우리 RGB 중앙값 / 유사성=배경 거리 P99.5+여유 /
--   매끄러움=인물 P1이 알파 0.95 / 유출=인물 P2-유사성 / 배경 비율 30% 미만이면 거부
-- v0.2 2단계 경로: 스크린 색 픽셀이 부족하거나 키 군집이 30% 미만이면 전체 픽셀 최빈 색을 등급(ac_screen)으로 판정하는
--   일반 경로("paper")로 재시도한다(녹색·파랑 스크린 색·피부색·옅은 색은 거부). 일반 경로 비율 하한은 BG_RATIO_MIN_PAPER.
--   등급 사유는 최빈 색 군집이 BG_RATIO_MIN 이상일 때만 쓴다(미만이면 '배경이 충분히 보이지 않음').
-- 박스거리 = OBS chroma_key_filter_v2 의 GetBoxFilteredChromaDist (선형 공간 쌍선형 -> sRGB 재인코딩).
-- 쌍선형 반 픽셀 샘플은 이웃 두 픽셀의 선형 평균과 같으므로 그렇게 계산한다.
--
-- 사용: params, info = tune.autotune(rgba, w, h, roi_top) / tune.autotune_hybrid(...)(+despill_strength·회전 설정 값, ac_tune_despill)
--   info.path = "v01"|"paper", 옷 색 충돌로 despill 을 끄면 info.despill_off=true, info.despill_reason="clothes"
--   실패 시 nil, 사유 문구. rgba 는 0부터 인덱싱되는 uint8 RGBA 버퍼(ffi cdata).
local ffi = require("ffi")
local tm = require("ac_tune_math")  -- 순수 계산 헬퍼(sRGB LUT·백분위 등)
local screen = require("ac_screen")  -- 색 등급·회전 행렬
local despill = require("ac_tune_despill")  -- 키 방향 despill 규칙

local M = {}

-- ---- 상수 (sim/constants.py 와 같은 값. 바꾸면 파이썬도 같이) ----
local CB_V4 = { -0.100644, -0.338572, 0.439216, 0.501961 }
local CR_V4 = { 0.439216, -0.398942, -0.040274, 0.501961 }
local MASK_EXP = 1.5
local SLIDER_SCALE, SLIDER_MIN, SLIDER_MAX = 1000, 1, 1000
local unpack = table.unpack or unpack
local CBCR_HIST_BINS = 256
local KEY_PEAK_RADIUS = 0.05
local BG_CLUSTER_MAX_DIST = 0.08
local BG_PERCENTILE = 99.5
local SIM_MARGIN, SIM_MIN = 0.005, 0.01
local PERSON_GAP = 0.06
local SMOOTH_PERCENTILE, SPILL_PERCENTILE = 1.0, 2.0
local SMOOTH_ALPHA_TARGET = 0.95
local SCREEN_DOMINANCE_MARGIN = 20  -- 스크린 색 픽셀 판정 여유(0~255 정수 단위). 근거는 sim/constants.py 주석
local BG_RATIO_MIN, MIN_PERSON_PIXELS, GAP_OVERLAP_RATIO_MAX = 0.30, 50, 0.5
local BG_RATIO_MIN_PAPER = 0.15
local KEY_PATH_V01, KEY_PATH_PAPER = "v01", "paper"
local FALLBACK_SMOOTHNESS, FALLBACK_SPILL = 0.08, 0.10
local HYBRID_DESPILL_STRENGTH = 1.0
local HYBRID_DESPILL_OFF = 0.0  -- 옷 색이 키 방향과 겹칠 때(파랑 배경 + 청바지 등) despill 을 끄고 알파만 사용
local KEY_COLOR_TYPE_CUSTOM = "custom"
local OBS_ALPHA_OPAQUE = 0xFF000000
local MSG_BG_NOT_VISIBLE = "배경이 충분히 보이지 않음"
local MSG_COLOR_OVERLAP = "옷 색이 배경과 겹침"

-- ---- 구현 상수 ----
local BYTES = tm.BYTES
local KIND_ALL = "all"  -- 일반 경로: 모든 픽셀을 후보로 센다
local BOX_SAMPLE_WEIGHT, BOX_CENTER_WEIGHT, BOX_WEIGHT_SUM = 2.0, 1.0, 9.0
local DIST_ALL_LOW = -1       -- 하한 없음(거리는 항상 0 이상)
local DIST_ALL_HIGH = 1e9     -- 상한 없음
local encode, round_half_even = tm.encode, tm.round_half_even
local median_from_hist, build_linear_planes = tm.median_from_hist, tm.build_linear_planes
local subset_percentiles, count_dist = tm.subset_percentiles, tm.count_dist

local function slider(value)
  return math.min(SLIDER_MAX, math.max(SLIDER_MIN, round_half_even(value * SLIDER_SCALE)))
end

-- 키색 RGB(0~255) -> (Cb, Cr). 계수 마지막 항(0.50196)은 w=1 곱
local function cbcr_from_rgb(r, g, b)
  local rn, gn, bn = r / 255, g / 255, b / 255
  return rn * CB_V4[1] + gn * CB_V4[2] + bn * CB_V4[3] + CB_V4[4],
         rn * CR_V4[1] + gn * CR_V4[2] + bn * CR_V4[3] + CR_V4[4]
end

-- ---- ① 키색 추정 ----

-- 스크린 색 픽셀 판정: 녹색 우세(G-max(R,B) > 여유) 또는 파랑 우세. kind = "green"|"blue"
local function is_screen_pixel(kind, r, g, b)
  if kind == KIND_ALL then return true end
  if type(kind) == "function" then return kind(r, g, b) end  -- 진단(ac_diag)이 넘기는 사용자 정의 후보 판정
  if kind == "green" then return g - math.max(r, b) > SCREEN_DOMINANCE_MARGIN end
  return b - math.max(r, g) > SCREEN_DOMINANCE_MARGIN
end

-- ROI 안 녹색 우세·파랑 우세 픽셀 수를 세어 많은 쪽(동수면 녹색)을 스크린 색으로 택한다. -> kind, 개수
local function pick_screen_kind(rgba, w, h, roi_top)
  local green, blue = 0, 0
  for p = roi_top * w, w * h - 1 do
    local i = p * BYTES
    local r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
    if is_screen_pixel("green", r, g, b) then green = green + 1 end
    if is_screen_pixel("blue", r, g, b) then blue = blue + 1 end
  end
  if blue > green then return "blue", blue end
  return "green", green
end

-- CbCr 히스토그램의 최빈 칸 중심 (numpy argmax: 앞쪽 칸 우선). 스크린 색 픽셀만 센다
local function find_cbcr_peak(rgba, w, h, roi_top, kind)
  local hist = ffi.new("uint32_t[?]", CBCR_HIST_BINS * CBCR_HIST_BINS)
  local last = CBCR_HIST_BINS - 1
  for p = roi_top * w, w * h - 1 do
    local i = p * BYTES
    local r, g, b = rgba[i] / 255, rgba[i + 1] / 255, rgba[i + 2] / 255
    local cb = r * CB_V4[1] + g * CB_V4[2] + b * CB_V4[3] + CB_V4[4]
    local cr = r * CR_V4[1] + g * CR_V4[2] + b * CR_V4[3] + CR_V4[4]
    if is_screen_pixel(kind, rgba[i], rgba[i + 1], rgba[i + 2]) and cb >= 0 and cb <= 1 and cr >= 0 and cr <= 1 then
      local ib = math.min(math.floor(cb * CBCR_HIST_BINS), last)
      local ic = math.min(math.floor(cr * CBCR_HIST_BINS), last)
      hist[ib * CBCR_HIST_BINS + ic] = hist[ib * CBCR_HIST_BINS + ic] + 1
    end
  end
  local best, best_idx = -1, 0
  for idx = 0, CBCR_HIST_BINS * CBCR_HIST_BINS - 1 do
    if hist[idx] > best then best, best_idx = hist[idx], idx end
  end
  local ib, ic = math.floor(best_idx / CBCR_HIST_BINS), best_idx % CBCR_HIST_BINS
  return (ib + 0.5) / CBCR_HIST_BINS, (ic + 0.5) / CBCR_HIST_BINS
end

-- kind 픽셀 중 CbCr 최빈 봉우리 반경 안 픽셀의 RGB 채널별 중앙값 -> r,g,b 정수 (없으면 nil)
local function peak_median_rgb(rgba, w, h, roi_top, kind)
  local ccb, ccr = find_cbcr_peak(rgba, w, h, roi_top, kind)
  local ch = ffi.new("uint32_t[?]", 3 * 256)
  local n = 0
  for p = roi_top * w, w * h - 1 do
    local i = p * BYTES
    local r, g, b = rgba[i], rgba[i + 1], rgba[i + 2]
    local cb, cr = cbcr_from_rgb(r, g, b)
    local dcb, dcr = cb - ccb, cr - ccr
    if is_screen_pixel(kind, r, g, b) and math.sqrt(dcb * dcb + dcr * dcr) <= KEY_PEAK_RADIUS then
      ch[r], ch[256 + g], ch[512 + b] = ch[r] + 1, ch[256 + g] + 1, ch[512 + b] + 1
      n = n + 1
    end
  end
  if n == 0 then return nil end
  return round_half_even(median_from_hist(ch, 0, n)),
         round_half_even(median_from_hist(ch, 256, n)),
         round_half_even(median_from_hist(ch, 512, n))
end

-- v0.1 키색: 스크린 색 픽셀의 봉우리 중앙값 -> r,g,b 정수.
-- 스크린 색 픽셀이 ROI의 BG_RATIO_MIN 미만이거나 없으면 nil (옷 색 겹침 판정보다 먼저 '배경 부족' 거부)
local function estimate_key_rgb(rgba, w, h, roi_top)
  local kind, count = pick_screen_kind(rgba, w, h, roi_top)
  if count == 0 or count < BG_RATIO_MIN * (h - roi_top) * w then return nil end
  return peak_median_rgb(rgba, w, h, roi_top, kind)
end

-- ---- ② 박스 거리 ----

-- roi_top 이상 행의 박스 필터 CbCr 거리 (float 배열, 개수). 이웃 샘플은 이미지 전체에서 가져온다(가장자리 Clamp)
local function compute_dist(rgba, w, h, roi_top, key_cb, key_cr)
  local lr, lg, lb = build_linear_planes(rgba, w, h)
  local n = (h - roi_top) * w
  local dist = ffi.new("float[?]", n)
  local cb1, cb2, cb3, cb4 = CB_V4[1], CB_V4[2], CB_V4[3], CB_V4[4]
  local cr1, cr2, cr3, cr4 = CR_V4[1], CR_V4[2], CR_V4[3], CR_V4[4]
  -- 이웃 두 픽셀 선형 평균 -> sRGB -> 키와의 CbCr 거리
  local function pair_dist(i1, i2)
    local r = encode((lr[i1] + lr[i2]) * 0.5)
    local g = encode((lg[i1] + lg[i2]) * 0.5)
    local b = encode((lb[i1] + lb[i2]) * 0.5)
    local dcb = r * cb1 + g * cb2 + b * cb3 + cb4 - key_cb
    local dcr = r * cr1 + g * cr2 + b * cr3 + cr4 - key_cr
    return math.sqrt(dcb * dcb + dcr * dcr)
  end
  local k = 0
  for y = roi_top, h - 1 do
    local row, rowm, rowp = y * w, math.max(y - 1, 0) * w, math.min(y + 1, h - 1) * w
    for x = 0, w - 1 do
      local xm, xp = math.max(x - 1, 0), math.min(x + 1, w - 1)
      local i = (row + x) * BYTES
      local dcb = rgba[i] / 255 * cb1 + rgba[i + 1] / 255 * cb2 + rgba[i + 2] / 255 * cb3 + cb4 - key_cb
      local dcr = rgba[i] / 255 * cr1 + rgba[i + 1] / 255 * cr2 + rgba[i + 2] / 255 * cr3 + cr4 - key_cr
      local center = math.sqrt(dcb * dcb + dcr * dcr)
      -- 샘플 오프셋 (-1,-.5) (1,.5) (-.5,1) (.5,-1) = 각각 이웃 두 픽셀의 평균
      local s = pair_dist(rowm + xm, row + xm) + pair_dist(row + xp, rowp + xp)
              + pair_dist(rowp + xm, rowp + x) + pair_dist(rowm + x, rowm + xp)
      dist[k] = (BOX_SAMPLE_WEIGHT * s + BOX_CENTER_WEIGHT * center) / BOX_WEIGHT_SUM
      k = k + 1
    end
  end
  return dist, n
end

-- ---- ③④ 인물 후보 ----
-- 반환: p1, p2 / 후보 부족이면 nil, nil / 옷 색 겹침이면 nil, nil, 사유
local function person_values(dist, n, similarity)
  local n_out = count_dist(dist, n, similarity, false)
  local n_person = count_dist(dist, n, similarity + PERSON_GAP, false)
  if n_out > 0 and (n_out - n_person) / n_out > GAP_OVERLAP_RATIO_MAX then
    return nil, nil, MSG_COLOR_OVERLAP
  end
  if n_person < MIN_PERSON_PIXELS then return nil, nil end
  local q = subset_percentiles(dist, n, similarity + PERSON_GAP, DIST_ALL_HIGH,
    { SMOOTH_PERCENTILE, SPILL_PERCENTILE })
  return q[1], q[2]
end

-- ---- 일반 경로(색지) 키색 ----

-- 키색이 v0.1 의 스크린 색(녹색 또는 파랑이 나머지 채널보다 마진 초과 우세)에 해당하는가
local function is_screen_color(r, g, b)
  return math.max(g - math.max(r, b), b - math.max(r, g)) > SCREEN_DOMINANCE_MARGIN
end

-- ROI 만 잘라 넘긴 박스 거리로 본 키색 군집 비율 (파이썬 box_chroma_dist(roi) 와 같은 가장자리 처리)
local function roi_cluster_ratio(rgba, w, h, roi_top, key_cb, key_cr)
  local dist, n = compute_dist(rgba + roi_top * w * BYTES, w, h - roi_top, 0, key_cb, key_cr)
  return count_dist(dist, n, BG_CLUSTER_MAX_DIST, true) / n
end

-- 전체 픽셀 최빈 색을 등급으로 판정한다. 성공 { r, g, b } / 거부 nil, 사유.
-- 거부: 녹색·파랑 스크린 색(v0.1 사유 유지) / 피부색·옅은 색(최빈 색 군집이 화면의 BG_RATIO_MIN 이상이면 사유 = 등급 문구,
-- 미만이면 MSG_BG_NOT_VISIBLE)
local function paper_key_rgb(rgba, w, h, roi_top)
  local r, g, b = peak_median_rgb(rgba, w, h, roi_top, KIND_ALL)
  if r == nil or is_screen_color(r, g, b) then return nil, MSG_BG_NOT_VISIBLE end
  local grade, reason = screen.key_grade(r, g, b)
  if grade ~= screen.GRADE_REJECT then return { r, g, b } end
  local key_cb, key_cr = cbcr_from_rgb(r, g, b)
  if roi_cluster_ratio(rgba, w, h, roi_top, key_cb, key_cr) >= BG_RATIO_MIN then return nil, reason end
  return nil, MSG_BG_NOT_VISIBLE
end

-- 키색 + 박스 거리 + 군집 비율 묶음
local function build_ctx(rgba, w, h, roi_top, key, path)
  local key_cb, key_cr = cbcr_from_rgb(key[1], key[2], key[3])
  local dist, n = compute_dist(rgba, w, h, roi_top, key_cb, key_cr)
  return { key = key, path = path, key_cb = key_cb, key_cr = key_cr, dist = dist, n = n,
           bg_ratio = count_dist(dist, n, BG_CLUSTER_MAX_DIST, true) / n }
end

-- 입력 검사 후 키색·거리를 만든다. 실패 시 nil, 사유
local function prepare(rgba, w, h, roi_top)
  if rgba == nil or w <= 0 or h <= 0 or roi_top < 0 or roi_top >= h then return nil, MSG_BG_NOT_VISIBLE end
  local kr, kg, kb = estimate_key_rgb(rgba, w, h, roi_top)
  local ctx
  if kr ~= nil then
    ctx = build_ctx(rgba, w, h, roi_top, { kr, kg, kb }, KEY_PATH_V01)
  end
  if ctx == nil or ctx.bg_ratio < BG_RATIO_MIN then
    -- 스크린 색 픽셀이 부족하거나(남은 녹색 소품 등) 키 군집이 작으면 최빈 색 일반 경로로 본다.
    -- v0.1 경로였는데 일반 경로가 거부하면 원래 사유(MSG_BG_NOT_VISIBLE)를 유지한다
    local key, reason = paper_key_rgb(rgba, w, h, roi_top)
    if key == nil then return nil, (ctx == nil) and reason or MSG_BG_NOT_VISIBLE end
    ctx = build_ctx(rgba, w, h, roi_top, key, KEY_PATH_PAPER)
  end
  local ratio_min = (ctx.path == KEY_PATH_PAPER) and BG_RATIO_MIN_PAPER or BG_RATIO_MIN
  if ctx.bg_ratio < ratio_min then return nil, MSG_BG_NOT_VISIBLE end
  return ctx
end

-- 자동 맞춤 본체. 성공 params, info, ctx / 실패 nil, 사유
local function run(rgba, w, h, roi_top)
  local ctx, why = prepare(rgba, w, h, roi_top)
  if ctx == nil then return nil, why end
  local dist, n = ctx.dist, ctx.n
  local bg_q = subset_percentiles(dist, n, DIST_ALL_LOW, BG_CLUSTER_MAX_DIST, { BG_PERCENTILE })
  local similarity = math.max(bg_q[1] + SIM_MARGIN, SIM_MIN)
  local p1, p2, overlap = person_values(dist, n, similarity)
  if overlap then return nil, overlap end

  local smoothness, spill = FALLBACK_SMOOTHNESS, FALLBACK_SPILL
  if p1 ~= nil then
    smoothness = (p1 - similarity) / (SMOOTH_ALPHA_TARGET ^ (1 / MASK_EXP))
    spill = p2 - similarity
  end
  local kr, kg, kb = unpack(ctx.key)
  local params = {
    key_color_type = KEY_COLOR_TYPE_CUSTOM,
    key_color = OBS_ALPHA_OPAQUE + kb * 65536 + kg * 256 + kr,
    similarity = slider(similarity),
    smoothness = slider(smoothness),
    spill = slider(spill),
  }
  local info = { bg_ratio = ctx.bg_ratio, similarity = similarity, smoothness = smoothness,
                 spill = spill, key_cb = ctx.key_cb, key_cr = ctx.key_cr, path = ctx.path }
  return params, info, ctx
end

-- 내장 v2 용 자동 맞춤. 성공 params, info / 실패 nil, 사유
function M.autotune(rgba, w, h, roi_top)
  local params, info = run(rgba, w, h, roi_top or 0)
  return params, info
end

-- 자동 키어용: v2 자동값 + despill_strength + 회전 설정 값 (sim/hybrid_key.py autotune_hybrid)
-- 녹색 우세 키: 강도 1·회전 없음(v0.1 그대로). 그 밖의 키: 키 방향 despill 강도 1, 단 인물 색이 키 방향과 겹치면
-- (파란 배경 + 청바지) 강도 0, info.despill_off=true, info.despill_reason="clothes" (호출자가 경고 로그)
function M.autotune_hybrid(rgba, w, h, roi_top)
  roi_top = roi_top or 0
  local params, info, ctx = run(rgba, w, h, roi_top)
  if params == nil then return nil, info end
  local rule = despill.decide(rgba, w, h, roi_top, ctx.key, ctx, info.similarity + PERSON_GAP)
  params.despill_strength = rule.clothes and HYBRID_DESPILL_OFF or HYBRID_DESPILL_STRENGTH
  for key, value in pairs(rule.params) do params[key] = value end
  info.despill_off = rule.clothes
  if rule.clothes then info.despill_reason = despill.REASON_CLOTHES end
  return params, info
end

-- ac_diag(진단)가 같은 키색 추정·박스 거리를, 진입점이 색지 경로 표기(KEY_PATH_PAPER)를 쓰도록 공개한다
M.estimate_key_rgb, M.peak_median_rgb, M.compute_dist, M.KIND_ALL = estimate_key_rgb, peak_median_rgb, compute_dist, KIND_ALL
M.KEY_PATH_PAPER = KEY_PATH_PAPER
return M
