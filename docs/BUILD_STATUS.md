# 빌드 및 검증 상태

기록일: 2026-10-10

- Windows x64 Release 컴파일·링크 성공: MSVC 19.44.35229, Windows SDK 10.0.26100.0, Qt 6.8.3.
- 마지막 빌드 로그: build/build-output.log. C++ 컴파일러·링커 경고/오류 없음.
- Vulkan 헤더 탐색 안내는 사용하지 않는 선택적 Qt 기능이며 이 Widgets 앱의 빌드를 막지 않음.
- PowerShell 스크립트 구문 분석 오류 없음. README.md 내용 및 SHA-256 유지.
- 실행 파일 SHA-256: 8F16FFDAE558C2B12D4FDCF909787B2168B13DE46A01328947420B07B31AA929
- 앱 실행, 오디오 장치 열기, 녹음 시험, 자동·단위·통합·E2E·수동 런타임 시험은 수행하지 않음.
- 내부 지연 20 ms 목표, Bluetooth/통화 앱/게임 호환성, 장시간 안정성 및 복구 결과는 사용자 최종 시험 전까지 미검증.

배포 실행 파일: dist/Speaker/Speaker.exe
배포 ZIP: dist/Speaker-1.0.0-windows-x64.zip
최종 시험: FINAL_TEST.md

## 배포 정적 확인

- DLL 누락 후속 수정: `build/Speaker.exe` 옆에도 Qt Core/Gui/Widgets, 플랫폼 플러그인, 한국어 번역, C++ 런타임을 배치했다. 이후 `scripts/build.ps1` 실행 시 자동 배치한다. `build` 런타임 PE 19개 정적 import 검사에서 미해결 의존성 0개, 배포 스크립트 구문 오류 0개. 앱 시작은 실행하지 않았으므로 사용자가 확인해야 한다.

- 배포 ZIP 생성 성공. 실행 폴더와 빌드 산출물의 EXE SHA-256 일치.
- 배포 PE 파일(EXE/DLL) 19개 import table 검사: 현재 Windows 시스템 DLL과 배포 DLL 기준 미해결 정적 의존성 0개.
- 한국어 번역, qwindows 플랫폼 플러그인, C++ 런타임 포함 확인.
- 이러한 정적 확인은 앱 시작·장치 연결·음성 처리 성공을 의미하지 않는다.

## 배포 구성

Qt DLL 및 Windows 플러그인, 한국어 번역, Microsoft C++ app-local 런타임,
원문 라이선스와 Qt 6.8.3 소스, 사용자 시험 안내를 포함한다.
코드 서명 인증서는 적용하지 않았다. VB-CABLE 드라이버는 별도 설치 대상이며 재배포하지 않는다.
