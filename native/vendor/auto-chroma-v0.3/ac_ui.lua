-- ac_ui.lua — '지금 화면 점검' 버튼의 실행 흐름·문구 (진입점 auto_chroma.lua 가 ac_flow 를 거쳐 부른다)
-- 흐름: (ac_flow 가 동기로 읽은 평균 프레임) -> 저장된 배경판 불러오기(BMP, 읽기만) -> 분석 제외 자르기 -> 진단(ac_diag) -> 상태 문구·로그 한 줄.
-- 필터 설정은 읽지도 바꾸지도 않는다. 진입점의 상태·로그 함수는 env 로 받는다(이 모듈은 OBS 에 직접 의존하지 않는다).
-- 화면 문구: 진단 사유의 옛 버튼 안내는 v0.3 에 없으므로 화면에서만 '자동 맞춤' 안내로 바꾼다(SCREEN_REPLACES). 진단 상수·로그는 그대로.
local diag = require("ac_diag")
local roi_mod = require("ac_roi")

local M = {}

local MSG_DIAG_FAIL = "점검하지 못했습니다 (OBS 로그 확인)"
local STATUS_SEP, LOG_SEP = " · ", " | "   -- 상태 문구는 '방식 · 등급 · 사유', 로그는 사유끼리 ' | '
local REASON_SEP = " / "
local METHOD_LABELS = { [diag.METHOD_COLOR] = "색 키 방식", [diag.METHOD_PLATE] = "배경판 방식", [diag.METHOD_NONE] = "진단 불가" }
local GRADE_LABELS = { [diag.GRADE_GOOD] = "좋음", [diag.GRADE_WARN] = "주의", [diag.GRADE_BAD] = "불가" }
-- 화면 대응표(원문 -> 화면 문구). 원문은 ac_diag.lua 의 DIAG_MSG_* 와 같은 글자여야 한다(plain 찾기)
local SCREEN_TEXT = "'자동 맞춤'이 배경판 방식으로 지웁니다"
local SCREEN_RETAKE = "'자동 맞춤'이나 '사람만 남기기'를 다시 누르세요"
local SCREEN_REPLACES = {
  { "'배경판 찍기' 방식으로 지우세요", SCREEN_TEXT },
  { "'배경판 찍기' 방식을 쓰세요", SCREEN_TEXT },
  { "배경판을 다시 찍으세요", SCREEN_RETAKE },
  { "배경판 다시 찍기를 권합니다", SCREEN_RETAKE },
}
local PALE_MARK = "너무 옅습니다(흰·회·검 계열)"   -- ac_diag.lua DIAG_MSG_PALE 의 일부: 흰 배경 안내를 띄우는 표지

-- 진단 사유 문구 -> 화면 문구 (대응표에 없는 문구는 그대로). 사유가 여러 개 이어진 문장이라 모든 짝을 모든 곳에 바꾼다
function M.screen_text(text)
  for _, pair in ipairs(SCREEN_REPLACES) do
    local out, from = "", 1
    while true do
      local s, e = text:find(pair[1], from, true)
      if s == nil then break end
      out = out .. text:sub(from, s - 1) .. pair[2]
      from = e + 1
    end
    text = out .. text:sub(from)
  end
  return text
end

-- 사유 중에 '배경색이 너무 옅다(흰·회·검 계열)'가 있는가 (흰 배경 안내용)
function M.is_pale(result)
  for _, reason in ipairs(result.reasons) do
    if reason:find(PALE_MARK, 1, true) ~= nil then return true end
  end
  return false
end

-- 화면 상태 문구: 방식 · 등급 · 사유
function M.status_text(result)
  return table.concat({ METHOD_LABELS[result.method], GRADE_LABELS[result.grade],
    M.screen_text(table.concat(result.reasons, REASON_SEP)) }, STATUS_SEP)
end

-- 로그 한 줄(진입점이 "[ac] " 머리말을 붙인다): 진단: method=<color|plate|none> grade=<good|warn|bad> 사유=<…>
function M.log_text(result)
  return string.format("진단: method=%s grade=%s 사유=%s", result.method, result.grade, table.concat(result.reasons, LOG_SEP))
end

-- 저장된 배경판 BMP -> RGBA 버퍼. 없거나 읽지 못하거나 프레임과 크기가 다르면 nil (점검만 건너뛴다)
function M.load_plate(plate_mod, name, w, h)
  return plate_mod.load_rgba(name, w, h)
end

-- 아래·왼·오른쪽을 잘라 낸 화면·배경판. 0 이면 같은 버퍼. 실패 nil, 사유 / 성공 rgba, w, h, plate_rgba
local function crop_inputs(roi, frame, plate_rgba)
  local rgba, w, h = roi_mod.crop_lrb(frame.rgba, frame.w, frame.h, roi[2], roi[3], roi[4])
  if rgba == nil then return nil, w end   -- 실패면 두 번째 값이 사유
  local cropped_plate = plate_rgba and (roi_mod.crop_lrb(plate_rgba, frame.w, frame.h, roi[2], roi[3], roi[4])) or nil
  return rgba, w, h, cropped_plate
end

-- 평균 프레임 한 장을 진단한다. env = { plate, roi={위,아래,왼,오른쪽}, log, warn, set_status }
function M.diagnose_frame(env, name, frame)
  local plate_rgba = M.load_plate(env.plate, name, frame.w, frame.h)
  local rgba, w, h, cropped_plate = crop_inputs(env.roi, frame, plate_rgba)
  if rgba == nil then return env.set_status(w) end
  local ok, result = pcall(diag.diagnose, rgba, w, h, env.roi[1], cropped_plate)
  if not ok then
    env.warn("%s", tostring(result))  -- 상세는 OBS 로그에만
    return env.set_status(MSG_DIAG_FAIL)
  end
  env.log("%s", M.log_text(result))
  env.set_status(M.status_text(result))
end

return M
