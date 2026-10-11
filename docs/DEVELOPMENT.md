# 개발 및 배포 기록

README.md는 사용자 요청에 따라 수정하지 않았다. PLAN.md가 구현 범위의 기준이며, 실행 검증은 사용자의 최종 시험으로 남긴다.

## 고정 개발 환경

- Windows 11 x64, C++20, MSVC toolset 14.44.35207 (cl 19.44.35229)
- Windows SDK 10.0.26100.0
- Qt 6.8.3, `msvc2022_64`, 동적 링크 Core/Gui/Widgets
- CMake 3.31.6, Ninja 1.11.1, aqtinstall 3.3.0
- 로컬 도구: `.tools/`, Microsoft Build Tools: `C:\BuildTools`

현재 PC의 개발 의존성은 에이전트가 설치했다. 사용자는 개발 환경을 설정할 필요 없이 `dist/Speaker/Speaker.exe`를 실행한다. 외부로 옮길 때는 exe만 복사하지 말고 배포 폴더 전체 또는 ZIP을 사용한다.

개발자가 다시 빌드하는 명령(최종 사용자 시험에는 불필요):

```powershell
.\scripts\build.ps1
.\scripts\package.ps1
```

빌드 스크립트는 컴파일·링크 후 `deploy-runtime.ps1`로 Qt DLL, Windows 플랫폼 플러그인, 한국어 번역 및 C++ 런타임을 `build` 폴더에 배치한다. 따라서 `build/Speaker.exe`도 직접 실행할 수 있는 구성을 갖춘다. 패키징도 같은 배포 함수를 사용한다. CMake의 컴파일러 및 헤더 기능 확인은 컴파일 작업이며, 앱 또는 오디오 테스트를 실행하지 않는다. `ctest`나 테스트 실행 대상은 등록하지 않았다.

## 구현 경계

- `AudioEngine`: WASAPI 공유 모드, Windows SRC 형식 변환, 프로세스 포함 트리 캡처. 장치 준비는 별도 제어 작업에서 하고 서비스 생성/버퍼 접근/해제는 오디오 스레드에서 수행한다.
- 입력/출력 큐는 제한된 크기이며, 클록 편차는 완만한 샘플 보간으로 보정한다. 지연 누적 시 오래된 샘플을 버리고 누락 횟수를 표시한다. 5 ms 혼합 단위와 무선 장치의 종단 지연은 다르다.
- `Recorder`: 사전 할당 SPSC 큐에서 디스크 스레드로 전달한다. 48 kHz PCM16 스테레오 WAV를 3시간마다 나눈다. 정상 파일은 헤더 확정 후 이름을 변경하며, 실패 시 복구용 원본을 보존한다.
- `MainWindow`: 시작 시 네 기능 OFF, 설정 저장, 세대 번호로 비동기 상태 구분, 경로별 오류 처리, 한국어 상태 표시.
- 설정은 `HKEY_CURRENT_USER\Software\SpeakerLocal\Speaker`, 녹음 목록과 오류 로그는 `%LOCALAPPDATA%\SpeakerLocal\Speaker`에 보관한다. 계정·서버·텔레메트리·오디오 내용 로그는 없다.

## 정적 검토와 실행 검증

컴파일 및 패키징 결과는 `BUILD_STATUS.md`에 기록한다. 앱 실행, 자동/단위/통합/E2E/수동 런타임 테스트, 마이크 캡처 및 장치 성능 측정은 수행하지 않는다. 전체 최종 시험은 `FINAL_TEST.md`의 한 절차를 사용한다.

## 구현 근거

- [Microsoft 프로세스 loopback](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/): 선택 프로세스와 하위 프로세스의 소리를 캡처한다.
- [IAudioCaptureClient](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudiocaptureclient), [IAudioRenderClient](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudiorenderclient): 서비스 획득과 Release의 스레드 제약을 지킨다.
- [Qt 라이선스](https://doc.qt.io/qt-6.8/licensing.html): 동적 Qt DLL, 원문 고지 및 해당 버전 소스를 배포에 포함한다.
- [VB-CABLE 공식 페이지](https://vb-audio.com/Cable/): 별도 드라이버이며 관리자 설치 및 재부팅이 필요할 수 있다.
