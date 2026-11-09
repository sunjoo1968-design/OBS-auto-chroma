-- SunjooAn / 0.4.0-dev
-- Private worker VM adapter. The original calculation modules remain unchanged.
-- C++ supplies read-only frame pointers, a UTF-8 module reader and cancellation.
local builtin_loader = package.loaders[1]
package.path, package.cpath = "", ""
package.loaders = { builtin_loader, function(name)
  local source, reason = _ac_read_module(name)
  if not source then return "\n\t" .. (reason or "모듈을 읽지 못했습니다") end
  local chunk, err = loadstring(source, "@" .. name .. ".lua")
  if not chunk then error(err, 0) end
  return chunk
end }

local ffi = require("ffi")
local roi_mod = require("ac_roi")
local plate_math = require("ac_plate_math")

local function checkpoint()
  if _ac_cancelled() then error("__AC_CANCELLED__", 0) end
end

local function rejected(message)
  return { success = false, message = message or "계산 결과를 만들지 못했습니다" }
end

local function plate_numbers()
  local numbers = { sigma_scale = plate_math.SIGMA_SCALE, ee_min = plate_math.EE_MIN }
  for key, value in pairs(plate_math.PARAM_DEFAULTS) do numbers[key] = value end
  for key, value in pairs(plate_math.SHADER_CONSTS) do numbers[key] = value end
  return numbers
end

return function(frames, w, h, roi, mode)
  checkpoint()
  local invalid = roi_mod.check(w, h, roi)
  if invalid then return rejected(invalid) end
  local acc = plate_math.new_accumulator(w, h)
  local size = w * h * 4
  local sum = mode == "auto" and ffi.new("uint16_t[?]", size) or nil
  for _, pointer in ipairs(frames) do
    checkpoint()
    local rgba = ffi.cast("const uint8_t *", pointer)
    plate_math.accumulate(acc, rgba)
    if sum then
      for i = 0, size - 1 do sum[i] = sum[i] + rgba[i] end
    end
  end
  checkpoint()
  if mode == "plate" then
    local e, sigma = plate_math.plate_stats(acc)
    checkpoint()
    local plate = plate_math.pack_plate(e, sigma, w, h)
    checkpoint()
    local warn = require("ac_plate_warn").person_warning(plate, nil, w, h)
    checkpoint()
    return {
      success = true, method = "plate", numbers = plate_numbers(),
      strings = { plate_kind = "manual" }, rgba = ffi.string(plate, size),
      message = warn and "빈 배경을 학습했습니다. 사람으로 보이는 영역이 있습니다. 적용 전에 확인하세요."
        or "빈 배경을 학습했습니다. 학습할 때 있던 진열대와 소품도 지워집니다. 적용 전에 확인하세요."
    }
  end

  -- Match ac_capture.average, including its integer rounding, before diagnosis.
  local averaged = ffi.new("uint8_t[?]", size)
  local half = math.floor(#frames / 2)
  for i = 0, size - 1 do averaged[i] = math.floor((sum[i] + half) / #frames) end
  checkpoint()
  local view, vw, vh = roi_mod.crop_lrb(averaged, w, h, roi[2], roi[3], roi[4])
  local diag = require("ac_diag")
  local result = diag.diagnose(view, vw, vh, roi[1], nil)
  checkpoint()
  local ui = require("ac_ui")
  local message = ui.status_text(result)
  if result.method == diag.METHOD_NONE then return rejected(message) end
  if result.method == diag.METHOD_COLOR then
    local params, info = require("ac_tune").autotune_hybrid(view, vw, vh, roi[1])
    checkpoint()
    if not params then return rejected(info) end
    local numbers, strings = {}, {}
    for key, value in pairs(params) do
      if type(value) == "number" then numbers[key] = value
      elseif type(value) == "string" then strings[key] = value end
    end
    if info.despill_reason == "clothes" then
      message = message .. " / 옷 색을 보호하기 위해 배경색 번짐 제거를 껐습니다"
    end
    return { success = true, method = "color", numbers = numbers,
      strings = strings, message = message }
  end
  local plate, info = require("ac_autoplate").compute(averaged, w, h, roi, acc)
  checkpoint()
  if not plate then return rejected(info) end
  if ui.is_pale(result) then
    message = message .. " / 흰 배경과 같은 색 소품도 지워질 수 있습니다. 보호 영역을 지정하고 확인하세요."
  end
  return { success = true, method = "plate", numbers = plate_numbers(),
    strings = { plate_kind = "auto" }, rgba = ffi.string(plate, size), message = message }
end
