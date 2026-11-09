-- ac_plate_warn.lua — 배경판 찍기 때 '화면에 사람이 보이는 것 같습니다' 경고 판단 (순수 Lua, OBS 의존 없음)
-- 이전에 저장된 같은 크기의 판이 있으면 새 판과의 차이 덩어리로 판단한다(ac_person, 진단과 같은 기준).
-- 이전 판이 없거나, 있어도 새 판이 이전 판과 많이 달라 결론이 안 나면(카메라·조명이 바뀐 경우) 피부색 휴리스틱을 쓴다.
-- 경고만 하고 저장은 막지 않으므로 휴리스틱이 틀려도 해가 작다.
local ffi = require("ffi")
local person = require("ac_person")

local M = {}

local BYTES = 4
-- 휴리스틱(8비트 YCbCr, BT.601): 피부색 상자 안이면서 배경 중앙값 색과 GAP 이상 다른 픽셀
local SKIN_CB_MIN, SKIN_CB_MAX, SKIN_CR_MIN, SKIN_CR_MAX = 77, 127, 133, 173
local PERSON_CHROMA_GAP = 10
local PERSON_MIN_PIXELS = 50
local PERSON_MIN_RATIO = 0.002
local CHROMA_BINS, CHROMA_CENTER = 256, 128

-- ---- 피부색 휴리스틱 ----
local function chroma(r, g, b)
  local cb = CHROMA_CENTER + (-0.168736 * r - 0.331264 * g + 0.5 * b)
  local cr = CHROMA_CENTER + (0.5 * r - 0.418688 * g - 0.081312 * b)
  return cb, cr
end

local function hist_median(hist, total)
  local cum, half = 0, total / 2
  for v = 0, CHROMA_BINS - 1 do
    cum = cum + hist[v]
    if cum >= half then return v end
  end
  return CHROMA_CENTER
end

-- 배경 중앙값 색과 다른 피부색 픽셀 수 (RGBA 버퍼, 알파 무시)
local function person_candidates(rgba, w, h)
  local n = w * h
  local cb_hist, cr_hist = {}, {}
  for v = 0, CHROMA_BINS - 1 do cb_hist[v], cr_hist[v] = 0, 0 end
  local cbs, crs = ffi.new("uint8_t[?]", n), ffi.new("uint8_t[?]", n)
  for p = 0, n - 1 do
    local cb, cr = chroma(rgba[p * BYTES], rgba[p * BYTES + 1], rgba[p * BYTES + 2])
    local icb, icr = math.min(255, math.max(0, math.floor(cb + 0.5))), math.min(255, math.max(0, math.floor(cr + 0.5)))
    cbs[p], crs[p] = icb, icr
    cb_hist[icb], cr_hist[icr] = cb_hist[icb] + 1, cr_hist[icr] + 1
  end
  local med_cb, med_cr = hist_median(cb_hist, n), hist_median(cr_hist, n)
  local count = 0
  for p = 0, n - 1 do
    local cb, cr = cbs[p], crs[p]
    local skin = cb >= SKIN_CB_MIN and cb <= SKIN_CB_MAX and cr >= SKIN_CR_MIN and cr <= SKIN_CR_MAX
    if skin and math.abs(cb - med_cb) + math.abs(cr - med_cr) > PERSON_CHROMA_GAP then count = count + 1 end
  end
  return count
end

-- 판에 사람 후보가 있는가
function M.has_person_heuristic(rgba, w, h)
  return person_candidates(rgba, w, h) >= math.max(PERSON_MIN_PIXELS, math.floor(w * h * PERSON_MIN_RATIO))
end

-- ---- 경고 판단 ----
-- new_plate: 방금 만든 판(RGBA), previous: 이전에 저장된 같은 크기의 판 또는 nil. 반환 true = 사람이 보이는 것 같다
function M.person_warning(new_plate, previous, w, h)
  if previous ~= nil then
    local ok, r = pcall(person.analyze, new_plate, previous, w, h)
    if ok and (r.plate_person or r.plate_ok) then return r.plate_person end  -- 결론 남: 덩어리 있음 / 이전 판과 같음
  end
  return M.has_person_heuristic(new_plate, w, h)
end

return M
