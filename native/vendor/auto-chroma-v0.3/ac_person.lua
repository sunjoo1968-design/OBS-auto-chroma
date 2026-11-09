-- ac_person.lua — 배경판 차이 덩어리로 사람 유무를 판정하고 배경판 신선도를 점검한다 (OBS 의존 없는 순수 Lua)
-- 정답 원본: sim/plate_person.py. 수정 시 tests/lua/run_parity.sh 필수(사람 여부·점검 결과 완전 일치, 비율 <= 1e-3).
-- 배경판이 있고 화면과 크기가 같을 때만 쓴다(ac_diag 가 부르고, ac_plate 가 '사람 찍힘' 경고에도 쓴다).
--   전경 = plate_key 알파 > CHECK_ALPHA. 전경 비율 <= CHECK_MAX -> 사람 없음·판 정상.
--   그 외: 긴 변 DIAG_PERSON_GRID_LONG_PX 이하 격자의 4-이웃 주 덩어리(최대의 REL_MIN 배 이상인 덩어리 합집합)가 전경 칸의
--   BLOB_SHARE 이상, 격자의 BLOB_MAX_AREA 이하이고 전경 비율 >= MIN_FG 면 '사람 있음' 후보. 덩어리(+번짐 폭) 밖의 전경 비율이
--   CHECK_MAX 를 넘으면 사람이 아니라 판이 낡은 것(사람 없음 + 판 낡음)으로 본다.
--   v0.2.1 D11: 주 덩어리가 격자 가장자리에 닿고 전경 칸의 EDGE_BLOB_SHARE 이상이면 하한이 EDGE_MIN_FG 로 내려가고,
--   그 경우에는 전경 비율 <= CHECK_MAX 여도 사람 판정을 한다(화면 끝에 걸친 작은 사람. 흩어진 가장자리 잡티는 제외).
--   v0.2.2 D14: 주 덩어리 칸 안 픽셀 중 알파 > PLATE_BLOB_SOLID_ALPHA 인 비율(단단함)이 BLOB_SOLID_MIN 미만이면 사람 아님(옅게 흩어진 조명 덩어리).
--   [30] 재튜닝: 개별 주 덩어리 가로 폭(바운딩 박스/격자 폭)의 최댓값이 MAX_BLOB_WIDTH 이상이면 사람 후보 아님(가로 띠).
-- 사용: r = person.analyze(img, plate, w, h)  -- r = { plate_ok, plate_fg_ratio, plate_cd_mean, plate_person, plate_blob_share }
-- 상수는 sim/constants.py 와 같은 값(한 줄 한 선언: tests/test_constants_sync.py).
local ffi = require("ffi")
local plate_math = require("ac_plate_math")

local M = {}

local DIAG_PERSON_GRID_LONG_PX, DIAG_PERSON_CELL_FG_SHARE = 160, 0.5
local DIAG_PERSON_BLOB_REL_MIN, DIAG_PERSON_BLOB_SHARE = 0.2, 0.6
local DIAG_PERSON_MIN_FG, DIAG_PERSON_BLOB_MAX_AREA = 0.03, 0.7
local DIAG_PERSON_MARGIN_CELLS = 2
local DIAG_PERSON_EDGE_MIN_FG, DIAG_PERSON_EDGE_BLOB_SHARE = 0.02, 0.9
local DIAG_PERSON_MAX_BLOB_WIDTH = 0.8
local DIAG_PERSON_BLOB_SOLID_MIN, PLATE_BLOB_SOLID_ALPHA = 0.70, 0.95  -- v0.2.2 D14 단단함

local NEIGHBORS = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }  -- 4-이웃

-- 전경 마스크(uint8[n]) 와 전경 개수. 전경 = 알파 > CHECK_ALPHA
local function fg_mask(alpha, n)
  local fg, count = ffi.new("uint8_t[?]", n), 0
  for i = 0, n - 1 do
    if alpha[i] > plate_math.CHECK_ALPHA then fg[i] = 1; count = count + 1 end
  end
  return fg, count
end

-- 전경 마스크 -> 격자 전경 칸(uint8[gw*gh]), 칸 변 px, gw, gh. 칸 안 전경 비율 >= CELL_FG_SHARE 면 전경 칸(가장자리 블록은 실제 픽셀 수)
local function fg_cells(fg, w, h)
  local step = math.floor((math.max(w, h) + DIAG_PERSON_GRID_LONG_PX - 1) / DIAG_PERSON_GRID_LONG_PX)
  local gw, gh = math.floor((w + step - 1) / step), math.floor((h + step - 1) / step)
  local counts = ffi.new("int32_t[?]", gw * gh)
  for y = 0, h - 1 do
    local row = math.floor(y / step) * gw
    for x = 0, w - 1 do
      if fg[y * w + x] == 1 then
        local c = row + math.floor(x / step)
        counts[c] = counts[c] + 1
      end
    end
  end
  local cells = ffi.new("uint8_t[?]", gw * gh)
  for cy = 0, gh - 1 do
    local bh = math.min(step, h - cy * step)
    for cx = 0, gw - 1 do
      local size = bh * math.min(step, w - cx * step)
      if counts[cy * gw + cx] >= DIAG_PERSON_CELL_FG_SHARE * size then cells[cy * gw + cx] = 1 end
    end
  end
  return cells, step, gw, gh
end

-- 4-이웃 연결 덩어리 라벨(int32[gw*gh], 0 = 전경 아님)과 덩어리별 크기 목록
local function label_blobs(cells, gw, gh)
  local labels, sizes = ffi.new("int32_t[?]", gw * gh), {}
  local stack = ffi.new("int32_t[?]", gw * gh)
  for start = 0, gw * gh - 1 do
    if cells[start] == 1 and labels[start] == 0 then
      local id, top, size = #sizes + 1, 1, 0
      stack[0], labels[start] = start, id
      while top > 0 do
        top = top - 1
        local c = stack[top]
        size = size + 1
        local cx, cy = c % gw, math.floor(c / gw)
        for _, d in ipairs(NEIGHBORS) do
          local nx, ny = cx + d[1], cy + d[2]
          if nx >= 0 and nx < gw and ny >= 0 and ny < gh then
            local nc = ny * gw + nx
            if cells[nc] == 1 and labels[nc] == 0 then labels[nc] = id; stack[top] = nc; top = top + 1 end
          end
        end
      end
      sizes[id] = size
    end
  end
  return labels, sizes
end

-- 주 덩어리 마스크(uint8[gw*gh]) 와 칸 수: 크기가 최대 덩어리의 REL_MIN 배 이상인 덩어리의 합집합
local function main_blobs(cells, gw, gh)
  local labels, sizes = label_blobs(cells, gw, gh)
  local biggest = 0
  for _, s in ipairs(sizes) do biggest = math.max(biggest, s) end
  local main, count = ffi.new("uint8_t[?]", gw * gh), 0
  for c = 0, gw * gh - 1 do
    local id = labels[c]
    if id > 0 and sizes[id] >= DIAG_PERSON_BLOB_REL_MIN * biggest then main[c] = 1; count = count + 1 end
  end
  return main, count
end

-- 주 덩어리를 MARGIN_CELLS 칸(정사각 이웃)만큼 넓힌 마스크
local function grow(main, gw, gh)
  local grown, m = ffi.new("uint8_t[?]", gw * gh), DIAG_PERSON_MARGIN_CELLS
  for cy = 0, gh - 1 do
    for cx = 0, gw - 1 do
      if main[cy * gw + cx] == 1 then
        for ny = math.max(cy - m, 0), math.min(cy + m, gh - 1) do
          for nx = math.max(cx - m, 0), math.min(cx + m, gw - 1) do grown[ny * gw + nx] = 1 end
        end
      end
    end
  end
  return grown
end

-- 덩어리 밖 픽셀의 (개수, 전경 개수, CD 합)
local function outside_stats(grown, fg, cd, w, h, step, gw)
  local n_out, fg_out, cd_sum = 0, 0, 0.0
  for y = 0, h - 1 do
    local row = math.floor(y / step) * gw
    for x = 0, w - 1 do
      if grown[row + math.floor(x / step)] == 0 then
        local i = y * w + x
        n_out, fg_out, cd_sum = n_out + 1, fg_out + fg[i], cd_sum + cd[i]
      end
    end
  end
  return n_out, fg_out, cd_sum
end

-- 주 덩어리들 중 개별 덩어리(4-이웃) 가로 폭(바운딩 박스)의 최댓값 / 격자 폭 (파이썬 blob_width_share)
local function blob_width_share(main, gw, gh)
  local labels, sizes = label_blobs(main, gw, gh)
  local lo, hi = {}, {}
  for cy = 0, gh - 1 do
    for cx = 0, gw - 1 do
      local id = labels[cy * gw + cx]
      if id > 0 then
        if not lo[id] or cx < lo[id] then lo[id] = cx end
        if not hi[id] or cx > hi[id] then hi[id] = cx end
      end
    end
  end
  local widest = 0
  for id = 1, #sizes do widest = math.max(widest, hi[id] - lo[id] + 1) end
  return widest / gw
end

-- 주 덩어리가 격자 가장자리 칸을 포함하는가
local function touches_border(main, gw, gh)
  for cx = 0, gw - 1 do
    if main[cx] == 1 or main[(gh - 1) * gw + cx] == 1 then return true end
  end
  for cy = 0, gh - 1 do
    if main[cy * gw] == 1 or main[cy * gw + gw - 1] == 1 then return true end
  end
  return false
end

-- v0.2.2 D14 주 덩어리 단단함 = 덩어리 칸 안 픽셀 중 알파 > PLATE_BLOB_SOLID_ALPHA 인 비율. 칸이 없으면 0 (파이썬 blob_solidity)
local function blob_solidity(main, step, gw, alpha, w, h)
  local total, solid = 0, 0
  for y = 0, h - 1 do
    local row = math.floor(y / step) * gw
    for x = 0, w - 1 do
      if main[row + math.floor(x / step)] == 1 then
        total = total + 1
        if alpha[y * w + x] > PLATE_BLOB_SOLID_ALPHA then solid = solid + 1 end
      end
    end
  end
  return total > 0 and solid / total or 0.0
end

-- 격자 판정 -> 사람 후보 여부, 주 덩어리 마스크, 덩어리 비율, 가장자리 규칙 적용 여부(파이썬 analyze 의 candidate·edge)
local function judge_blob(fg, alpha, ratio, w, h)
  local cells, step, gw, gh = fg_cells(fg, w, h)
  local total = 0
  for c = 0, gw * gh - 1 do total = total + cells[c] end
  if total == 0 then return nil end
  local main, main_count = main_blobs(cells, gw, gh)
  local share = main_count / total
  local is_person = share >= DIAG_PERSON_BLOB_SHARE and main_count / (gw * gh) <= DIAG_PERSON_BLOB_MAX_AREA
    and blob_width_share(main, gw, gh) < DIAG_PERSON_MAX_BLOB_WIDTH
  if is_person then is_person = blob_solidity(main, step, gw, alpha, w, h) >= DIAG_PERSON_BLOB_SOLID_MIN end
  local edge = is_person and touches_border(main, gw, gh) and share >= DIAG_PERSON_EDGE_BLOB_SHARE
  local candidate = is_person and ratio >= (edge and DIAG_PERSON_EDGE_MIN_FG or DIAG_PERSON_MIN_FG)
  return { main = main, share = share, step = step, gw = gw, gh = gh, edge = edge, candidate = candidate }
end

-- 화면·배경판(크기 같음, RGBA8) -> 점검 결과 표
function M.analyze(img, plate, w, h)
  local n = w * h
  local alpha, mean_cd, cd = plate_math.plate_key(img, plate, w, h)
  local fg, fg_count = fg_mask(alpha, n)
  local ratio = fg_count / n
  local result = { plate_ok = ratio <= plate_math.CHECK_MAX, plate_fg_ratio = ratio, plate_cd_mean = mean_cd,
    plate_person = false, plate_blob_share = 0.0 }
  if fg_count == 0 then return result end
  local b = judge_blob(fg, alpha, ratio, w, h)
  if not b then return result end
  if ratio <= plate_math.CHECK_MAX and not (b.edge and b.candidate) then return result end
  result.plate_blob_share = b.share
  if not b.candidate then return result end
  local n_out, fg_out, cd_sum = outside_stats(grow(b.main, b.gw, b.gh), fg, cd, w, h, b.step, b.gw)
  local out_ratio = n_out > 0 and fg_out / n_out or 0.0
  if out_ratio > plate_math.CHECK_MAX then return result end  -- 덩어리 밖도 어긋남 = 사람이 아니라 판이 낡음
  result.plate_person, result.plate_ok, result.plate_fg_ratio = true, true, out_ratio
  result.plate_cd_mean = n_out > 0 and cd_sum / n_out or 0.0
  return result
end

return M
