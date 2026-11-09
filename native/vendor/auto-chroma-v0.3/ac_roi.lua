-- ac_roi.lua — 분석 제외 4방향 값 확인·잘라 내기 (PLAN-v0.3 D23). OBS 의존 없음.
-- 정답 원본: sim/roi.py (check 문구·범위 상수 동일). 수정 시 tests/lua/parity_autoplate.lua --roi 필수.
-- 위쪽은 기존 진단·튜닝의 roi_top 인자가 처리한다. 여기서는 값 확인(check)과 아래·왼·오른쪽만 잘라 낸 사본(crop_lrb)을 만든다.
-- 세 값이 0 이면 사본 없이 같은 버퍼(cdata)를 돌려준다 -> v0.2.2 와 비트 동일.
local ffi = require("ffi")

local M = {}

local ROI_MIN_PX = 0
local ROI_MAX_PX = 10000
local ROI_REJECT_MSG = "분석 제외 값이 화면보다 큽니다"
local ROI_RANGE_MSG = "분석 제외 값은 0~10000 사이 정수여야 합니다"
local RGBA_BYTES = 4
local ROI_FIELDS = 4

M.MIN_PX, M.MAX_PX, M.REJECT_MSG, M.RANGE_MSG = ROI_MIN_PX, ROI_MAX_PX, ROI_REJECT_MSG, ROI_RANGE_MSG

local function is_int(v) return type(v) == "number" and v == math.floor(v) and v == v end

-- roi = { 위, 아래, 왼쪽, 오른쪽 }. 문제 없으면 nil, 거절이면 사유 문구
function M.check(w, h, roi)
  if type(roi) ~= "table" or #roi ~= ROI_FIELDS then return ROI_RANGE_MSG end
  for i = 1, ROI_FIELDS do
    local v = roi[i]
    if not is_int(v) or v < ROI_MIN_PX or v > ROI_MAX_PX then return ROI_RANGE_MSG end
  end
  if roi[1] + roi[2] >= h or roi[3] + roi[4] >= w then return ROI_REJECT_MSG end
  return nil
end

-- 아래·왼·오른쪽을 잘라 낸 RGBA 사본. 셋 다 0 이면 같은 cdata. 반환: buf, w', h' / 너무 크면 nil, 사유
function M.crop_lrb(rgba, w, h, bottom, left, right)
  if bottom == 0 and left == 0 and right == 0 then return rgba, w, h end
  if bottom < 0 or left < 0 or right < 0 or bottom >= h or left + right >= w then return nil, ROI_REJECT_MSG end
  local nw, nh = w - left - right, h - bottom
  local out = ffi.new("uint8_t[?]", nw * nh * RGBA_BYTES)
  for y = 0, nh - 1 do
    ffi.copy(out + y * nw * RGBA_BYTES, rgba + (y * w + left) * RGBA_BYTES, nw * RGBA_BYTES)
  end
  return out, nw, nh
end

return M
