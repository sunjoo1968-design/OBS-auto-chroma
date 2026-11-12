# Windows 빌드 및 검증

자동 크로마키 1.4 · 제작자 SunjooAn

| 대상 | OBS SDK | Qt |
|---|---|---|
| OBS 32 | 32.0.0 | 6.8.3 |
| OBS 33 | 33.0.1 | 6.11.1 |

Visual Studio 2022 Build Tools의 C++ 데스크톱 도구, Windows SDK, CMake 3.28 이상을 준비합니다. OBS 버전별로 해당 Windows x64 런타임의 bin/64bit 폴더를 지정합니다. SDK 준비 스크립트는 공식 OBS 소스와 Qt 배포 파일의 SHA-256을 확인하고, 제공한 OBS DLL에 맞는 import library를 생성합니다.

```powershell
$obs32 = 'C:/path/to/obs32/bin/64bit'
$obs33 = 'C:/path/to/obs33/bin/64bit'
./scripts/prepare-deps.ps1 -ObsMajor 32 -RuntimeDirectory $obs32
./scripts/prepare-deps.ps1 -ObsMajor 33 -RuntimeDirectory $obs33
./scripts/build.ps1 -ObsMajor 32 -LuaLibrary (Join-Path $obs32 'lua51.dll')
./scripts/build.ps1 -ObsMajor 33 -LuaLibrary (Join-Path $obs33 'lua51.dll')
./scripts/package.ps1
```

준비한 SDK는 .deps, 컴파일 결과는 build, 배포 ZIP과 SHA-256 목록은 dist에 생성됩니다. OBS 32와 OBS 33의 DLL 및 Qt SDK를 각각 사용합니다.

기본 검사는 분석·정밀 모델·장면 변환·미리보기 조작을 확인합니다. D3D11 통합 검사를 추가하려면 CMake 옵션 AC_BUILD_GPU_TESTS를 켜고 AC_GPU_RUNTIME_BIN과 AC_GPU_LIBOBS_DATA를 해당 OBS 버전의 런타임과 libobs 데이터 디렉터리로 지정합니다. 별도 샘플 PNG는 AC_SAMPLE_IMAGE로 지정할 수 있습니다.

1.4의 D3D11 통합 검사는 같은 필터에서 초기화·복원한 프레임, 확인된 미리보기의 요청 번호, 렌더링에 실패한 후보와 실제 출력의 분리를 확인합니다. 이 검사는 합성 카메라를 사용하는 별도 테스트 프로그램에서 실행합니다.

