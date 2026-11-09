-- ac_screen.lua — 색 키 후보 색의 극좌표 분류·키 방향 색상 회전 (sim/screen.py 의 Lua 이식, OBS 의존 없는 순수 Lua)
-- 정답 원본은 파이썬이다. 수정 시 tests/lua/run_parity.sh 를 반드시 통과시킬 것. 근거 문서: docs/skin-zone.md
--
-- 극좌표: OBS 셰이더 CbCr 기준 각도(0~360도)·채도. 등급: 피부 구역(문헌 구역 + 여유) 안 -> reject(피부색),
--   옅음 -> reject(옅음), 구역 경계 SKIN_WARN_DEG 이내 -> warn, 그 밖 -> support.
-- 색상 회전: 키색의 기하 색상을 녹색(120도)으로 돌리는 회색축 회전 행렬(3x3, 행 우선 9개 값의 테이블).
--
-- 사용: grade, reason = screen.key_grade(r, g, b) (0~255) / m = screen.despill_rotation_for_key(r, g, b) (녹색 우세·무채색이면 nil)
local M = {}

-- ---- 상수 (sim/constants.py 와 같은 값. 바꾸면 파이썬도 같이) ----
local CB_V4 = { -0.100644, -0.338572, 0.439216, 0.501961 }
local CR_V4 = { 0.439216, -0.398942, -0.040274, 0.501961 }
local CBCR_NEUTRAL = 0.501961
local SKIN_ANGLE_LO, SKIN_ANGLE_HI = 85.0, 178.2
local SKIN_MARGIN_DEG, SKIN_WARN_DEG = 15.0, 10.0
local CHROMA_MIN, CHROMA_NEUTRAL = 0.10, 0.03
local DEG_FULL_CIRCLE, HUE_GREEN_DEG = 360.0, 120.0
local GRADE_SUPPORT, GRADE_WARN, GRADE_REJECT = "support", "warn", "reject"
local MSG_KEY_SKIN = "배경색이 피부색과 비슷함"
local MSG_KEY_PALE = "배경색이 너무 옅음(흰·회·검 계열)"
local MSG_KEY_FAINT = "배경색이 너무 연함(연하늘·연노랑 같은 옅은 색)"
local MSG_KEY_NEAR_SKIN = "배경색이 피부색 구역 경계에 가까움"

-- ---- 구현 상수 ----
local BYTE_MAX = 255
local SQRT3 = math.sqrt(3.0)
local GREY_EPS = 1e-9  -- 기하 색상이 정의되지 않는 무채색 판정
local atan2 = math.atan2 or math.atan  -- LuaJIT(5.1): atan2 / 5.3 이상: atan(y, x)
-- 회색축 회전 행렬의 외적 행렬 S (행 우선)
local CROSS = { 0, -1, 1, 1, 0, -1, -1, 1, 0 }

M.GRADE_SUPPORT, M.GRADE_WARN, M.GRADE_REJECT = GRADE_SUPPORT, GRADE_WARN, GRADE_REJECT
M.MSG_KEY_PALE = MSG_KEY_PALE  -- ac_diag 가 거부 사유가 "옅음"인지 가린다
M.MSG_KEY_FAINT = MSG_KEY_FAINT  -- 연한 유채색 거부 사유

-- sRGB 0~1 -> (Cb, Cr). 계수 마지막 항은 w=1 곱
function M.cbcr(r, g, b)
  return r * CB_V4[1] + g * CB_V4[2] + b * CB_V4[3] + CB_V4[4],
         r * CR_V4[1] + g * CR_V4[2] + b * CR_V4[3] + CR_V4[4]
end

-- sRGB 0~255 -> (각도 0~360도, 채도)
function M.cbcr_polar(r, g, b)
  local cb, cr = M.cbcr(r / BYTE_MAX, g / BYTE_MAX, b / BYTE_MAX)
  local dcb, dcr = cb - CBCR_NEUTRAL, cr - CBCR_NEUTRAL
  return math.deg(atan2(dcr, dcb)) % DEG_FULL_CIRCLE, math.sqrt(dcb * dcb + dcr * dcr)
end

-- 원형 각도 거리(도)
local function circular_gap(a, b)
  local d = math.abs(a - b)
  return math.min(d, DEG_FULL_CIRCLE - d)
end

-- 확장 피부 구역 밖 각도 거리(도). 구역 안이면 0
local function outside_distance(angle)
  local lo, hi = SKIN_ANGLE_LO - SKIN_MARGIN_DEG, SKIN_ANGLE_HI + SKIN_MARGIN_DEG
  local a = angle % DEG_FULL_CIRCLE
  if a >= lo and a <= hi then return 0.0 end
  return math.min(circular_gap(a, lo), circular_gap(a, hi))
end

-- 각도(도)가 여유를 포함한 피부 구역 [LO-margin, HI+margin] 안인가
function M.in_skin_zone(angle) return outside_distance(angle) == 0.0 end

-- 색 키 후보 색(0~255)의 (등급, 사유). 판정 순서: 무채색 -> 피부 구역 안 -> 옅음 -> 경계 근접 -> 지원.
-- 무채색을 구역 판정보다 먼저 두는 이유: 채도가 0에 가까우면 각도가 무작위라 흰 벽·연회색이 우연히 구역 안에 든다
function M.key_grade(r, g, b)
  local angle, chroma = M.cbcr_polar(r, g, b)
  if chroma < CHROMA_NEUTRAL then return GRADE_REJECT, MSG_KEY_PALE end
  local dist = outside_distance(angle)
  if dist == 0.0 then return GRADE_REJECT, MSG_KEY_SKIN end
  if chroma < CHROMA_MIN then return GRADE_REJECT, MSG_KEY_FAINT end
  if dist < SKIN_WARN_DEG then return GRADE_WARN, MSG_KEY_NEAR_SKIN end
  return GRADE_SUPPORT, ""
end

-- 색지 후보 픽셀(0~255): 채도 >= CHROMA_MIN 이고 피부 구역 밖
function M.is_paper_candidate(r, g, b)
  local angle, chroma = M.cbcr_polar(r, g, b)
  return chroma >= CHROMA_MIN and not M.in_skin_zone(angle)
end

-- 기하 색상(도): atan2(sqrt3(G-B), 2R-G-B), 입력 sRGB 0~1. 빨강 0, 녹색 120, 파랑 240(=-120)
function M.geometric_hue(r, g, b)
  return math.deg(atan2(SQRT3 * (g - b), 2.0 * r - g - b))
end

-- 회색축 회전 행렬 M = cos*I + (1-cos)*J/3 + sin*S/sqrt3 (3x3, 행 우선 9개)
function M.hue_rotation(theta_deg)
  local t = math.rad(theta_deg)
  local c, s = math.cos(t), math.sin(t)
  local out = {}
  for row = 0, 2 do
    for col = 0, 2 do
      local identity = (row == col) and c or 0.0
      out[row * 3 + col + 1] = identity + (1.0 - c) / 3.0 + s * CROSS[row * 3 + col + 1] / SQRT3
    end
  end
  return out
end

-- 키색(0~255)의 색상을 녹색으로 돌리는 3x3. 녹색 우세(G > max(R,B), v0.1 규칙)·무채색이면 nil
function M.despill_rotation_for_key(r, g, b)
  if g > math.max(r, b) then return nil end
  local rn, gn, bn = r / BYTE_MAX, g / BYTE_MAX, b / BYTE_MAX
  if math.max(rn, gn, bn) - math.min(rn, gn, bn) < GREY_EPS then return nil end
  return M.hue_rotation(HUE_GREEN_DEG - M.geometric_hue(rn, gn, bn))
end

return M
