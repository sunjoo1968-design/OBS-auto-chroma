-- ac_diag.lua — 진단 '이 배경 어떻게 지울까?' (sim/diagnose.py 의 Lua 이식, OBS 의존 없는 순수 Lua)
-- 정답 원본은 파이썬이다. 수정 시 tests/lua/run_parity.sh 를 반드시 통과시킬 것(방식·등급·사유 문구 완전 일치, 특징량 <= 1e-3).
--
-- 사용: result = diag.diagnose(rgba, w, h, roi_top, plate)   -- plate = 같은 크기 RGBA8 배경판(ac_plate_math 형식) 또는 nil
--   result = { method = "color"|"plate"|"none", grade = "good"|"warn"|"bad", reasons = { 문구 <= 2개 }, features = {...} }
-- 흐름: extract_features(화면에서 특징 ①~⑧) -> judge(임계로 방식·등급·사유). rgba 는 0부터 인덱싱되는 uint8 RGBA 버퍼(ffi cdata).
-- 의존(단방향): ac_tune(키색 추정·박스 거리) · ac_tune_math(백분위) · ac_screen(색 등급) · ac_tune_despill(옷 색 충돌) · ac_plate_math(재촬영 기준) · ac_person(배경판 차이 덩어리 사람 판정·점검)
local ffi = require("ffi")
local tune = require("ac_tune")
local tm = require("ac_tune_math")
local screen = require("ac_screen")
local despill = require("ac_tune_despill")
local plate_math = require("ac_plate_math")
local plate_person = require("ac_person")

local M = {}

-- ---- 상수 (sim/constants.py 와 같은 값. 바꾸면 파이썬도 같이. 한 줄 한 선언: tests/test_constants_sync.py) ----
local BG_CLUSTER_MAX_DIST, BG_PERCENTILE, SIM_MARGIN, SIM_MIN, PERSON_GAP = 0.08, 99.5, 0.005, 0.01, 0.06
local BG_RATIO_MIN, BG_RATIO_MIN_PAPER, MIN_PERSON_PIXELS, GAP_OVERLAP_RATIO_MAX = 0.30, 0.15, 50, 0.5
local DEG_FULL_CIRCLE = 360.0
local REC709 = { 0.2126, 0.7152, 0.0722 }
local DIAG_METHOD_COLOR, DIAG_METHOD_PLATE, DIAG_METHOD_NONE = "color", "plate", "none"
local DIAG_GRADE_GOOD, DIAG_GRADE_WARN, DIAG_GRADE_BAD = "good", "warn", "bad"
local DIAG_REASONS_MAX = 2
local DIAG_RATIO_HARD, DIAG_RATIO_WARN = 0.10, 0.30
local DIAG_LIGHT_MIN, DIAG_SPREAD_MAX = 0.6, 0.08
local DIAG_HOLE_WARN, DIAG_HOLE_HARD = 0.01, 0.065
local DIAG_CLOTHES_MARGIN_MIN, DIAG_CHROMA_WARN = 0.03, 0.15
local DIAG_LIGHT_HUE_DEG, DIAG_LIGHT_CHROMA_FRAC = 15.0, 0.35
local DIAG_LUMA_PCT_LO, DIAG_LUMA_PCT_HI = 10.0, 90.0
local DIAG_SPREAD_PCT, DIAG_CLOTHES_PCT = 95.0, 10.0
local DIAG_HOLE_SPAN_DIV, DIAG_HOLE_SPAN_MIN_PX, DIAG_PCT_SCALE = 60.0, 3, 100.0
local DIAG_MSG_COLOR_OK = "이 배경은 색 키 방식으로 잘 지워집니다"
local DIAG_MSG_SKIN = "색 키 불가: 배경색이 피부색과 비슷합니다 — '배경판 찍기' 방식으로 지우세요"
local DIAG_MSG_PALE = "색 키 불가: 배경색이 너무 옅습니다(흰·회·검 계열) — '배경판 찍기' 방식을 쓰세요"
local DIAG_MSG_FAINT_PAPER = "색 키 불가: 배경색이 너무 연합니다(연한 색지) — '배경판 찍기' 방식을 쓰세요"
local DIAG_MSG_FEW = "색 키 불가: 배경색이 화면의 {pct}%만 보입니다 — '배경판 찍기' 방식을 쓰세요"
local DIAG_MSG_OVERLAP = "색 키 불가: 옷 색이 배경색과 겹칩니다 — '배경판 찍기' 방식을 쓰세요"
local DIAG_MSG_NEAR_SKIN = "주의: 배경색이 피부색에 가까워 얼굴이 조금 투명해질 수 있습니다"
local DIAG_MSG_SMALL = "주의: 배경이 화면의 {pct}%만 보입니다 — '자르기' 필터로 나머지를 가리세요"
local DIAG_MSG_LIGHT = "주의: 조명이 한쪽으로 어둡습니다 — 조명을 고르게 맞추세요"
local DIAG_MSG_SPREAD = "주의: 배경 색이 고르지 않습니다 — 주름과 그림자를 줄이세요"
local DIAG_MSG_HOLES_HARD = "색 키 불가: 배경이 이음새나 가려진 부분으로 많이 끊겨 있습니다 — '배경판 찍기' 방식을 쓰세요"
local DIAG_MSG_FAINT = "주의: 배경색이 옅은 편이라 인물 가장자리가 흐려질 수 있습니다"
local DIAG_MSG_HOLES = "주의: 배경에 이음새나 테이프 자국이 보입니다 — 한 장으로 펴 주세요"
local DIAG_MSG_CLOTHES = "주의: 옷 색이 배경색과 비슷합니다 — 다른 옷을 권합니다"
local DIAG_MSG_DESPILL = "주의: 옷 색이 배경색과 비슷해 번짐 제거를 끕니다"
local DIAG_MSG_EMPTY = "화면에 배경이 보이지 않아 진단할 수 없습니다"
local DIAG_MSG_PLATE_RETAKE = "카메라나 조명이 바뀌었습니다 — 배경판을 다시 찍으세요"
local DIAG_MSG_PLATE_ADVISE = "배경판과 지금 화면이 조금 다릅니다 — 배경판 다시 찍기를 권합니다"
local DIAG_MSG_PLATE_OK = "저장된 배경판이 지금 화면과 잘 맞습니다"
local DIAG_MSG_PLATE_LIGHT = "카메라나 조명이 조금 바뀌었습니다 — 화면에 잡티가 늘었으면 배경판을 다시 찍으세요"
local DIAG_PLATE_HARD_FG, DIAG_PLATE_HARD_CD = 0.04, 1.5  -- v0.2.2 D14 조명 변화('권장') 기준

-- ---- 구현 상수 ----
local BYTES = 4
local BYTE_MAX = 255
local HALF_CIRCLE = DEG_FULL_CIRCLE / 2
local FLOOR_RATIO = 1e-6          -- 0 나눗셈 방지 (파이썬 _FLOOR_RATIO)
local DIST_LOW, DIST_HIGH = -1, 1e9  -- tm.subset_percentiles 에서 모든 거리를 포함
local round_half_even = tm.round_half_even

M.METHOD_COLOR, M.METHOD_PLATE, M.METHOD_NONE = DIAG_METHOD_COLOR, DIAG_METHOD_PLATE, DIAG_METHOD_NONE
M.GRADE_GOOD, M.GRADE_WARN, M.GRADE_BAD = DIAG_GRADE_GOOD, DIAG_GRADE_WARN, DIAG_GRADE_BAD
M.THRESHOLDS = { ratio_hard = DIAG_RATIO_HARD, light_min = DIAG_LIGHT_MIN, spread_max = DIAG_SPREAD_MAX,
  clothes_min = DIAG_CLOTHES_MARGIN_MIN, hole_warn = DIAG_HOLE_WARN, hole_hard = DIAG_HOLE_HARD,
  ratio_warn = DIAG_RATIO_WARN, chroma_warn = DIAG_CHROMA_WARN }

-- ---- 키 선택 ----
-- 군집 묶음: 키색 + 박스 거리(ROI 만) + 군집 비율
local function cluster(roi, w, rh, key)
  local cb, cr = screen.cbcr(key[1] / BYTE_MAX, key[2] / BYTE_MAX, key[3] / BYTE_MAX)
  local dist, n = tune.compute_dist(roi, w, rh, 0, cb, cr)
  return { key = key, dist = dist, n = n, ratio = tm.count_dist(dist, n, BG_CLUSTER_MAX_DIST, true) / n }
end

-- kind(KIND_ALL 또는 후보 판정 함수) 픽셀의 최빈 봉우리 색으로 만든 군집
local function peak_cluster(roi, w, rh, kind)
  local r, g, b = tune.peak_median_rgb(roi, w, rh, 0, kind)
  if r == nil then return nil end
  return cluster(roi, w, rh, { r, g, b })
end

-- 색지 후보(채도 있고 피부 구역 밖) 픽셀의 화면 비율
local function candidate_ratio(roi, w, rh)
  local count = 0
  for p = 0, w * rh - 1 do
    if screen.is_paper_candidate(roi[p * BYTES], roi[p * BYTES + 1], roi[p * BYTES + 2]) then count = count + 1 end
  end
  return count / (w * rh)
end

-- v0.1 스크린 색 키(군집 BG_RATIO_MIN 이상) -> 전체 최빈 색(군집 BG_RATIO_MIN 이상) -> 색지 후보 최빈 색 (파이썬 _pick_key)
local function pick_key(roi, w, rh)
  local r, g, b = tune.estimate_key_rgb(roi, w, rh, 0)
  if r ~= nil then
    local c = cluster(roi, w, rh, { r, g, b })
    if c.ratio >= BG_RATIO_MIN then return c end
  end
  local all = peak_cluster(roi, w, rh, tune.KIND_ALL)
  if all.ratio >= BG_RATIO_MIN or candidate_ratio(roi, w, rh) < BG_RATIO_MIN_PAPER then return all end
  return peak_cluster(roi, w, rh, screen.is_paper_candidate)
end

-- ---- 군집 안 구멍 ----
-- 한 축 방향 줄마다: 칸 바로 앞 또는 뒤 span 칸 안에 군집이 모두 있으면 sand=1
local function mark_axis(in_bg, sand, lines, len, step, line_stride, span)
  local csum = ffi.new("int32_t[?]", len + 1)
  for ln = 0, lines - 1 do
    local base = ln * line_stride
    for i = 0, len - 1 do csum[i + 1] = csum[i] + in_bg[base + i * step] end
    for i = 0, len - 1 do
      local before = csum[i] - csum[math.max(i - span, 0)]
      local after = csum[math.min(i + 1 + span, len)] - csum[math.min(i + 1, len)]
      if before > 0 and after > 0 then sand[base + i * step] = 1 end
    end
  end
end

-- 군집 밖이지만 가로 또는 세로로 span 안 양쪽에 군집이 있는 칸 / (군집 + 구멍)
local function hole_ratio(in_bg, w, h)
  local span = math.max(DIAG_HOLE_SPAN_MIN_PX, round_half_even(math.max(w, h) / DIAG_HOLE_SPAN_DIV))
  local sand = ffi.new("uint8_t[?]", w * h)
  mark_axis(in_bg, sand, h, w, 1, w, span)
  mark_axis(in_bg, sand, w, h, w, 1, span)
  local holes, bg = 0, 0
  for i = 0, w * h - 1 do
    if in_bg[i] == 1 then bg = bg + 1 elseif sand[i] == 1 then holes = holes + 1 end
  end
  if bg + holes == 0 then return 0.0 end
  return holes / (bg + holes)
end

-- ---- 조명·인물 특징 ----
-- 조명 불균일을 재는 픽셀: 배경 군집 + 같은 색상(±DIAG_LIGHT_HUE_DEG)이면서 어두워진 배경(그림자). uint8 마스크
local function light_mask(roi, w, rh, in_bg, key_angle, key_chroma)
  local mask = ffi.new("uint8_t[?]", w * rh)
  for p = 0, w * rh - 1 do
    if in_bg[p] == 1 then
      mask[p] = 1
    else
      local angle, chroma = screen.cbcr_polar(roi[p * BYTES], roi[p * BYTES + 1], roi[p * BYTES + 2])
      local gap = math.abs((angle - key_angle + HALF_CIRCLE) % DEG_FULL_CIRCLE - HALF_CIRCLE)
      if gap <= DIAG_LIGHT_HUE_DEG and chroma >= DIAG_LIGHT_CHROMA_FRAC * key_chroma then mask[p] = 1 end
    end
  end
  return mask
end

-- 마스크 픽셀의 휘도(sRGB 가중합) P10/P90. 대상이 없으면 1
local function luma_ratio(roi, n, mask)
  local luma, count = ffi.new("float[?]", n), 0
  for p = 0, n - 1 do
    if mask[p] == 1 then
      luma[count] = (roi[p * BYTES] * REC709[1] + roi[p * BYTES + 1] * REC709[2] + roi[p * BYTES + 2] * REC709[3]) / BYTE_MAX
      count = count + 1
    end
  end
  if count == 0 then return 1.0 end
  local q = tm.subset_percentiles(luma, count, DIST_LOW, DIST_HIGH, { DIAG_LUMA_PCT_LO, DIAG_LUMA_PCT_HI })
  return q[1] / math.max(q[2], FLOOR_RATIO)
end

-- 인물 후보 특징. shadow(같은 색상의 어두운 배경)는 인물로 세지 않는다. -> { has_person, overlap, clothes_margin(없으면 nil) }
local function person_features(dist, n, in_bg, light, similarity)
  local persons, n_out, n_person = ffi.new("float[?]", n), 0, 0
  for k = 0, n - 1 do
    local d = dist[k]
    if d > similarity and not (light[k] == 1 and in_bg[k] == 0) then
      n_out = n_out + 1
      if d > similarity + PERSON_GAP then persons[n_person] = d; n_person = n_person + 1 end
    end
  end
  local feats = { has_person = n_person >= MIN_PERSON_PIXELS, overlap = n_out > 0 and (n_out - n_person) / n_out or 0.0 }
  if feats.has_person then
    local q = tm.subset_percentiles(persons, n_person, DIST_LOW, DIST_HIGH, { DIAG_CLOTHES_PCT })
    feats.clothes_margin = q[1] - similarity - PERSON_GAP
  end
  return feats
end

-- ---- 특징 추출 ----
-- 군집 거리 -> in_bg 마스크(uint8)
local function bg_mask(dist, n)
  local in_bg = ffi.new("uint8_t[?]", n)
  for k = 0, n - 1 do if dist[k] <= BG_CLUSTER_MAX_DIST then in_bg[k] = 1 end end
  return in_bg
end

-- 배경판 점검(배경판이 있고 크기가 같을 때). 사람 유무는 배경판 차이 덩어리로 판정한다(ac_person). feats 에 plate_* 를 채운다
local function plate_features(feats, roi, plate, roi_top, w, rh)
  for k, v in pairs(plate_person.analyze(roi, plate + roi_top * w * BYTES, w, rh)) do feats[k] = v end
end

-- 진단 특징량. 화면이 비면 { empty = true }
function M.extract_features(rgba, w, h, roi_top, plate)
  roi_top = roi_top or 0
  if rgba == nil or w <= 0 or h <= 0 or roi_top < 0 or roi_top >= h then return { empty = true } end
  local rh = h - roi_top
  local roi = rgba + roi_top * w * BYTES
  local n = w * rh
  local c = pick_key(roi, w, rh)
  local in_bg = bg_mask(c.dist, n)
  local q = tm.subset_percentiles(c.dist, n, DIST_LOW, BG_CLUSTER_MAX_DIST, { BG_PERCENTILE, DIAG_SPREAD_PCT })
  local similarity = q and math.max(q[1] + SIM_MARGIN, SIM_MIN) or SIM_MIN
  local grade = screen.key_grade(c.key[1], c.key[2], c.key[3])
  local angle, chroma = screen.cbcr_polar(c.key[1], c.key[2], c.key[3])
  local light = light_mask(roi, w, rh, in_bg, angle, chroma)
  local feats = { empty = false, key_rgb = c.key, key_grade = grade, key_angle = angle, key_chroma = chroma,
    bg_ratio = c.ratio, similarity = similarity, spread = q and q[2] or 0.0, light_ratio = luma_ratio(roi, n, light),
    hole_ratio = hole_ratio(in_bg, w, rh), despill_conflict = 0.0 }
  for k, v in pairs(person_features(c.dist, n, in_bg, light, similarity)) do feats[k] = v end
  if feats.has_person then
    local cb, cr = screen.cbcr(c.key[1] / BYTE_MAX, c.key[2] / BYTE_MAX, c.key[3] / BYTE_MAX)
    local dist = tune.compute_dist(rgba, w, h, roi_top, cb, cr)  -- 파이썬은 전체 화면에서 거리를 구해 ROI 만 본다
    feats.despill_conflict = despill.conflict_ratio_for(rgba, w, h, roi_top, c.key,
      { key_cb = cb, key_cr = cr, dist = dist }, similarity + PERSON_GAP)
  end
  if plate ~= nil then plate_features(feats, roi, plate, roi_top, w, rh) end
  return feats
end

-- ---- 판정 ----
local function pct_text(template, ratio)
  return (template:gsub("{pct}", tostring(round_half_even(ratio * DIAG_PCT_SCALE))))
end

local function result_of(method, grade, reasons, feats)
  local out = {}
  for i = 1, math.min(#reasons, DIAG_REASONS_MAX) do out[i] = reasons[i] end
  return { method = method, grade = grade, reasons = out, features = feats }
end

-- 배경판 방식: 색 키가 안 되는 이유(base) + (사람 없음·배경판 있음이면) 배경판 점검 결과
local function plate_result(feats, base)
  if feats.plate_ok == nil then return result_of(DIAG_METHOD_PLATE, DIAG_GRADE_GOOD, { base }, feats) end
  if not feats.plate_ok then
    if feats.plate_fg_ratio <= DIAG_PLATE_HARD_FG and feats.plate_cd_mean < DIAG_PLATE_HARD_CD then
      return result_of(DIAG_METHOD_PLATE, DIAG_GRADE_WARN, { DIAG_MSG_PLATE_LIGHT, base }, feats)  -- 조명만 바뀜
    end
    return result_of(DIAG_METHOD_PLATE, DIAG_GRADE_BAD, { DIAG_MSG_PLATE_RETAKE, base }, feats)
  end
  if feats.plate_cd_mean >= plate_math.RETAKE_CD then
    return result_of(DIAG_METHOD_PLATE, DIAG_GRADE_WARN, { DIAG_MSG_PLATE_ADVISE, base }, feats)
  end
  return result_of(DIAG_METHOD_PLATE, DIAG_GRADE_GOOD, { base, DIAG_MSG_PLATE_OK }, feats)
end

-- 색 키 방식의 주의 사유 목록 (순서 고정: 파이썬 judge 와 같다)
local function color_warnings(feats, t)
  local ratio, warns = feats.bg_ratio, {}
  local function add(cond, text) if cond then warns[#warns + 1] = text end end
  add(feats.key_grade == screen.GRADE_WARN, DIAG_MSG_NEAR_SKIN)
  add(ratio < t.ratio_warn, pct_text(DIAG_MSG_SMALL, ratio))
  add(feats.light_ratio < t.light_min, DIAG_MSG_LIGHT)
  add(feats.spread > t.spread_max, DIAG_MSG_SPREAD)
  add(feats.hole_ratio > t.hole_warn, DIAG_MSG_HOLES)
  add(feats.clothes_margin ~= nil and feats.clothes_margin < t.clothes_min, DIAG_MSG_CLOTHES)
  add(feats.despill_conflict > despill.CONFLICT_MAX, DIAG_MSG_DESPILL)
  add(feats.key_chroma < t.chroma_warn, DIAG_MSG_FAINT)
  return warns
end

-- 특징 -> 결과. thresholds 로 임계(M.THRESHOLDS 키)를 덮어쓸 수 있다
function M.judge(feats, thresholds)
  local t = {}
  for k, v in pairs(M.THRESHOLDS) do t[k] = (thresholds and thresholds[k]) or v end
  if feats.empty then return result_of(DIAG_METHOD_NONE, DIAG_GRADE_BAD, { DIAG_MSG_EMPTY }, feats) end
  local ratio = feats.bg_ratio
  if feats.key_grade == screen.GRADE_REJECT then
    local _, msg = screen.key_grade(feats.key_rgb[1], feats.key_rgb[2], feats.key_rgb[3])
    local reject_msg = DIAG_MSG_SKIN
    if msg == screen.MSG_KEY_PALE then reject_msg = DIAG_MSG_PALE end
    if msg == screen.MSG_KEY_FAINT then reject_msg = DIAG_MSG_FAINT_PAPER end
    return plate_result(feats, reject_msg)
  end
  if ratio < t.ratio_hard then return plate_result(feats, pct_text(DIAG_MSG_FEW, ratio)) end
  if feats.overlap > GAP_OVERLAP_RATIO_MAX then return plate_result(feats, DIAG_MSG_OVERLAP) end
  if feats.hole_ratio > t.hole_hard then return plate_result(feats, DIAG_MSG_HOLES_HARD) end
  local warns = color_warnings(feats, t)
  if #warns > 0 then return result_of(DIAG_METHOD_COLOR, DIAG_GRADE_WARN, warns, feats) end
  return result_of(DIAG_METHOD_COLOR, DIAG_GRADE_GOOD, { DIAG_MSG_COLOR_OK }, feats)
end

-- 화면(+배경판) -> 진단 결과. roi_top 위쪽 행(배너 등)은 보지 않는다
function M.diagnose(rgba, w, h, roi_top, plate)
  return M.judge(M.extract_features(rgba, w, h, roi_top, plate))
end

return M
