-- ac_tune_despill.lua — 키 방향 despill 규칙 (sim/hybrid_key.py 의 despill_conflict_ratio·autotune_hybrid 이식, 순수 Lua)
-- 정답 원본은 파이썬이다. 수정 시 tests/lua/run_parity.sh 를 반드시 통과시킬 것.
-- ac_tune.lua 만 require 한다(단방향). 키색이 녹색 우세가 아니면(보라·파랑·청록 배경지 등) 키색 색상을 녹색으로 돌리는
-- 회전 행렬(ac_screen)로 번짐을 줄인다. 인물(키와 먼 픽셀) 중 번짐 방향 색 비율이 DESPILL_CONFLICT_MAX 를 넘으면(옷 색 충돌) 끈다.
--
-- 키어 설정 값(ROTATE_KEYS 10개): despill_rotate(0|1) + 회전 행렬 9값(행 우선 rot_r1..rot_b3). 회전 없음이면 단위행렬.
local screen = require("ac_screen")

local M = {}

-- ---- 상수 (sim/constants.py 와 같은 값. 바꾸면 파이썬도 같이) ----
local DESPILL_EXCESS_MIN = 0.01568627450980392   -- 4/255: 키 방향 번짐 초과량 하한(sRGB 0~1)
local DESPILL_CONFLICT_MAX = 0.05
local DESPILL_REASON_CLOTHES = "clothes"
-- 키어 필터 설정 키 (ac_keyer.lua·ac_apply.lua 와 같은 목록. 한 줄 형식 유지: tests/test_constants_sync.py)
local ROTATE_KEYS = { "despill_rotate", "rot_r1", "rot_r2", "rot_r3", "rot_g1", "rot_g2", "rot_g3", "rot_b1", "rot_b2", "rot_b3" }

-- ---- 구현 상수 ----
local BYTES = 4
local BYTE_MAX = 255
local IDENTITY = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }
local MATRIX_SIZE = 9

M.ROTATE_KEYS = ROTATE_KEYS
M.REASON_CLOTHES = DESPILL_REASON_CLOTHES
M.CONFLICT_MAX = DESPILL_CONFLICT_MAX

-- 회전 행렬(9값 또는 nil) -> 키어 설정 값 테이블 (모든 키를 항상 채운다: 이전 적용의 값이 남지 않게)
local function rotation_params(matrix)
  local out = { [ROTATE_KEYS[1]] = matrix and 1 or 0 }
  for i = 1, MATRIX_SIZE do out[ROTATE_KEYS[i + 1]] = (matrix or IDENTITY)[i] end
  return out
end

-- 비녹색 키: 인물 후보(박스 거리 > limit 이고 자기 거리도 > limit) 중 회전 공간에서 번짐 초과가 DESPILL_EXCESS_MIN 을 넘는 비율.
-- dist = ac_tune 의 박스 거리(ROI 행부터, 개수 (h-roi_top)*w). 박스 필터는 경계를 1px 넓히므로 픽셀 자신의 거리도 함께 본다
-- (배경 경계가 인물로 세어지지 않게). 인물 후보가 없으면 0
local function conflict_ratio(rgba, w, h, roi_top, m, key_cb, key_cr, dist, limit)
  local persons, conflicts = 0, 0
  for k = 0, (h - roi_top) * w - 1 do
    if dist[k] > limit then
      local i = (roi_top * w + k) * BYTES
      local r, g, b = rgba[i] / BYTE_MAX, rgba[i + 1] / BYTE_MAX, rgba[i + 2] / BYTE_MAX
      local cb, cr = screen.cbcr(r, g, b)
      if math.sqrt((cb - key_cb) ^ 2 + (cr - key_cr) ^ 2) > limit then
        persons = persons + 1
        local rot_g = m[4] * r + m[5] * g + m[6] * b
        local rot_max = math.max(m[1] * r + m[2] * g + m[3] * b, m[7] * r + m[8] * g + m[9] * b)
        if rot_g - rot_max > DESPILL_EXCESS_MIN then conflicts = conflicts + 1 end
      end
    end
  end
  if persons == 0 then return 0.0 end
  return conflicts / persons
end

-- 진단용: 키색(r,g,b)의 옷 색 충돌 비율. 회전이 없는 키(녹색 우세·무채색)면 0. ctx = { key_cb, key_cr, dist }
function M.conflict_ratio_for(rgba, w, h, roi_top, key, ctx, limit)
  local matrix = screen.despill_rotation_for_key(key[1], key[2], key[3])
  if matrix == nil then return 0.0 end
  return conflict_ratio(rgba, w, h, roi_top, matrix, ctx.key_cb, ctx.key_cr, ctx.dist, limit)
end

-- despill 규칙 결정. key = { r, g, b }, ctx = { key_cb, key_cr, dist }, limit = 유사성 + 인물 간격
-- 반환 { clothes = 옷 색 충돌로 끔 여부, params = 키어 회전 설정 값 }
function M.decide(rgba, w, h, roi_top, key, ctx, limit)
  local matrix = screen.despill_rotation_for_key(key[1], key[2], key[3])
  if matrix == nil then return { clothes = false, params = rotation_params(nil) } end
  local ratio = conflict_ratio(rgba, w, h, roi_top, matrix, ctx.key_cb, ctx.key_cr, ctx.dist, limit)
  return { clothes = ratio > DESPILL_CONFLICT_MAX, params = rotation_params(matrix) }
end

return M
