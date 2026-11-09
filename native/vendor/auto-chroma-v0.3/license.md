# 라이선스 파생 범위 (v0.2)

> **이 문서는 법률 자문이 아닙니다.** 파일이 어디서 왔는지 기록한 사실과, 그 사실로 판단했을 때 보수적인 해석을 적은 메모입니다. 판매나 배포를 결정하기 전에 라이선스 전문가의 확인을 받으세요.

## 요약

- OBS Studio는 **GPLv2**(GNU General Public License 버전 2) 조건으로 배포됩니다. 정확한 조건은 OBS 저장소의 `COPYING`을 확인하세요.
- 이 프로젝트의 화면 처리 파일 `obs/ac_keyer.effect`는 OBS의 `chroma_key_filter_v2.effect`를 베이스로 고쳐 쓴 파일이라 **GPLv2 파생물**로 취급합니다.
- `obs/ac_plate_keyer.effect`는 새로 쓴 계산이지만, 주변 구조(고정 9샘플 박스 평균, 샘플 위치 계산)를 같은 OBS 파일에서 따왔으므로 **보수적으로 파생물**로 취급합니다.
- Lua 스크립트와 파이썬 시뮬레이터는 OBS 코드를 복사하지 않았습니다. 다만 같은 계산을 맞추려고 색 변환 계수를 인용했습니다.

## 파일별 파생 범위

| 파일 | OBS 원본과의 관계 | 취급 |
|---|---|---|
| `obs/ac_keyer.effect` | OBS 33.0.0 `plugins/obs-filters/data/chroma_key_filter_v2.effect`에서 출발. 알파 계산(고정 9샘플 박스 거리)과 premultiply 처리는 원본 그대로이고, 색 처리(despill)만 바꾸거나 더했습니다. 원본 사본은 `docs/ref/chroma_key_filter_v2.effect` | **파생. 파일 전체를 GPLv2로 취급** |
| `obs/ac_plate_keyer.effect` | 알고리즘(Horprasert 1999)은 자체 구현. 다만 `GetBoxFilteredDistortion`의 샘플 위치·가중치(중심 1 + 주변 4개 ×2, 합을 9로 나눔)가 v2 `GetBoxFilteredChromaDist`와 같은 구조 | **보수적으로 파생(GPLv2)**. 박스 평균 부분이 원본에서 왔다고 보고 파일 전체를 같은 조건으로 둠 |
| `obs/ac_keyer.lua`, `obs/ac_plate_keyer.lua` | OBS의 Lua 스크립트 API(`obslua`)를 호출하는 등록 코드. OBS 코드를 복사하지 않음. `Script Sources (Lua Only)` 문서의 `obs_register_source` 사용 방식을 따름 | 자체 작성. OBS는 API를 통해 호출될 뿐 링크하지 않음 |
| `obs/auto_chroma.lua`, `obs/ac_*.lua` 나머지 | 자체 작성. OBS API(`obslua`, `ffi`)를 호출. CbCr 변환 계수는 `plugins/obs-filters/chroma-key-filter.c`의 `cb_vec`, `cr_vec`(93~94줄, `-0.100644 -0.338572 0.439216 0.501961` / `0.439216 -0.398942 -0.040274 0.501961`) 값을 그대로 인용 | 자체 작성 + 계수 값 인용(저작물이 아닌 수치 상수로 보지만 아래 해석 참고) |
| `sim/*.py` | 파이썬 정답 시뮬레이터. 셰이더의 계산을 파이썬으로 옮김(포팅). `sim/obs_chroma.py`는 위 계수와 v2 알파 식을 파이썬으로 옮긴 것. `sim/plate_key.py`는 Horprasert 방식 | 자체 작성. v2 알파 식을 옮긴 부분(`sim/obs_chroma.py`)은 파생 가능성이 있어 보수적으로 같은 조건 |
| `docs/ref/chroma_key_filter_v2.effect`, `docs/ref/chroma-key-filter.c` | OBS 33.0.0 원본 사본(읽기 전용 참조). 배포 묶음에는 넣지 않음 | **OBS 원본 그대로, GPLv2.** 저장소 안에서만 보관 |
| `fixtures/web/*.png` | 외부 사진 11장(시험용). 출처·라이선스는 `fixtures/web/CREDITS.md` | 아래 '외부 사진' 참고 |
| 알고리즘 인용 | Horprasert et al., "A Statistical Approach for Real-time Robust Background Subtraction and Shadow Detection" (1999) | 논문 속 알고리즘 인용이며 코드 복사가 아님 |

해석 메모:

- 셰이더(`.effect`)는 OBS가 실행 중에 불러 컴파일하는 파일이라, 이 프로젝트에서 가장 직접적으로 OBS 코드와 닿는 부분입니다. 그래서 가장 보수적으로 둡니다.
- Lua 스크립트는 OBS가 불러 실행할 뿐 OBS 코드를 포함하지 않으므로 별도 라이선스를 쓸 여지가 있습니다. 다만 `.effect`와 한 묶음으로 배포되므로, 묶음 전체를 GPLv2로 두는 쪽이 단순합니다.
- 색 변환 계수는 숫자 상수라서 저작물성이 약할 수 있다고 보이지만, 판단은 전문가의 몫이므로 출처를 숨기지 않고 위처럼 적어 둡니다.

## 판매를 검토할 때의 의미

1. **`.effect` 파일을 배포하면 GPLv2 조건이 붙습니다.** 소스(셰이더 전체)를 받는 사람에게 공개해야 하고, 같은 GPLv2 조건으로 주어야 합니다. 셰이더는 텍스트 파일 그대로 배포되므로 소스가 함께 나가지만, 배포 묶음에 라이선스 전문과 원본 출처 고지를 넣어야 합니다. 이를 위해 배포 zip에 GPLv2 전문(`GPL-2.0.txt`)과 아래 'OBS 저작권 고지'를 포함합니다.
2. **묶음 전체를 폐쇄 소스로 팔기는 어렵습니다.** `.effect`가 GPLv2이고 Lua 스크립트와 한 묶음이므로, 보수적으로는 묶음 전체의 소스 공개가 필요하다고 봅니다. 돈을 받고 파는 것 자체는 GPLv2에서 금지되지 않습니다(소스 공개 조건을 지키면 됩니다).
3. **파생을 피하려면 셰이더를 처음부터 다시 써야 합니다.** OBS 코드를 보지 않고 같은 기능을 새로 쓰거나, 라이선스가 맞는 다른 소스를 쓰는 방법이 있습니다. 이 경우에도 전문가 검토가 필요합니다. 이 프로젝트에서 상품화 판단은 범위 밖이며, 필요하면 별도로 의사결정을 진행합니다.

## 배포 묶음에 들어가는 라이선스 문서

- `license.md` — 이 문서(파생 범위 기록)
- `GPL-2.0.txt` — GNU General Public License 버전 2 전문. 출처: https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt (2026-10-30 받음, SHA-256 `edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6`)

### OBS 저작권 고지

`ac_keyer.effect`는 OBS Studio(https://github.com/obsproject/obs-studio) 33.0.0의 `plugins/obs-filters/data/chroma_key_filter_v2.effect`를 베이스로 고쳐 쓴 파일이고, `ac_plate_keyer.effect`는 같은 파일의 샘플 위치·가중치 구조를 따랐습니다. OBS Studio의 저작권은 OBS Studio 기여자들에게 있고(정확한 저작권자 표기는 OBS 저장소의 `COPYING`과 각 소스 파일 머리말을 따르세요), GNU General Public License 버전 2 조건으로 배포됩니다. 이 두 `.effect` 파일에도 같은 GPLv2 조건이 적용되며, 파일 안에 있는 원본 출처 주석을 지우지 마세요. 원본 소스는 위 저장소의 33.0.0 태그에서 받을 수 있습니다.

## 외부 사진 출처

시험용 외부 사진 11장(`fixtures/web/c01.png`~`c11.png`)은 위키미디어 공용에서 받았고, 파일별 원제목·라이선스·URL은 `fixtures/web/CREDITS.md`에 있습니다. 라이선스는 Public domain, CC0, CC BY 2.0이 섞여 있습니다.

- CC BY 2.0 사진(c02, c09, c10)은 **저작자 표시**가 필요합니다. 시험 목적의 내부 사용이라 현재는 CREDITS.md에 출처를 기록하는 것으로 두었습니다.
- **배포 묶음에는 이 사진들을 넣지 않습니다.** `dist/auto-chroma-v0.2.zip`은 OBS 파일과 문서(`install.md`·`paper-guide.md`·`windows-check.md`)·`license.md`·`GPL-2.0.txt`만 포함합니다.
- 사용자가 직접 찍은 사진(`fixtures/user/`, `fixtures/user_v02/`)에는 외부 저작권이 없지만, 사람이 나온 사진은 초상권을 고려해 외부에 공유하지 마세요.

## 확인한 기준 자료

- `docs/ref/chroma_key_filter_v2.effect` — 출처: obsproject/obs-studio 태그 33.0.0, 받은 날 2026-10-29
- `docs/ref/chroma-key-filter.c` — 같은 태그, 같은 날. CbCr 계수 93~94줄
- `obs/ac_keyer.effect` 머리 주석: `베이스: obsproject/obs-studio 33.0.0 chroma_key_filter_v2.effect (docs/ref 사본과 diff 대조)`
