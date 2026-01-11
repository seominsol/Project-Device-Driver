# Project-Device-Driver


### 🎯 프로젝트 개요 (Project Overview)

본 프로젝트는 임베디드 리눅스 환경에서 다양한 하드웨어(OLED, DHT11, DS1302, Rotary Encoder)를 제어하기 위한 **리눅스 캐릭터 디바이스 드라이버(Linux Character Device Driver)**를 구현한 결과물입니다.

애플리케이션이 하드웨어 복잡성을 신경 쓰지 않고, 표준 파일 입출력 함수를 통해 센서를 제어할 수 있도록 커널 모듈을 설계했습니다.

### 🥅 프로젝트 목표 (Project Goals)

1.  **캐릭터 디바이스 드라이버 구현**
    * 각 센서를 Character Device로 커널에 등록하고, Major/Minor Number를 할당받아 관리
    * `file_operations` 구조체를 정의하여 유저 영역의 시스템 콜을 커널 영역의 하드웨어 제어 함수로 연결

2.  **하드웨어 제어 및 추상화**
    * **OLED/Sensors**: I2C 및 GPIO 서브시스템을 활용해 물리 계층을 제어
    * **Device Node**: `/dev/oled` 등의 장치 파일을 통해 유저 영역과 소통


---

## 🛠️ 사용 부품 및 하드웨어 구성

| 부품명 (Component) | 인터페이스 (Interface) | 설명 (Description) |
| :--- | :---: | :--- |
| **OLED Display** (SSD1306) | I2C | 시스템 상태 및 센서 데이터 시각화 |
| **DHT11** | GPIO (1-Wire) | 온도 및 습도 데이터 수집 |
| **DS1302 RTC** | GPIO (3-Wire) | 실시간 시간 정보 유지 및 제공 |
| **Rotary Encoder** | GPIO | 사용자 입력 처리 (메뉴 이동 및 값 조절) |

### 2. 하드웨어 연결도 (Circuit Diagram)

<img width="1193" height="671" alt="image" src="https://github.com/user-attachments/assets/87028366-b5d0-437c-baa3-a4f01afb1471" />


---

## ⚙️ 시스템 동작 흐름 (System Operation Flow)

**유한 상태 머신(FSM, Finite State Machine)** 구조로 설계되어 있으며, 로터리 엔코더(Rotary Encoder)의 입력(회전, 클릭)에 따라 **모니터링 모드**와 **시간 설정 모드**를 순환

### 1. 상태 천이도 (State Transition Diagram)


<img width="1897" height="1006" alt="image" src="https://github.com/user-attachments/assets/c713d6c7-743e-4569-8d52-58db2a807f22" />

### 2. 동작 모드 및 제어 (Operation Logic)

시스템은 크게 두 가지 모드로 동작, 로터리 엔코더를 통해 제어

#### 🟢 일반 모드 (SCREEN_NORMAL)
* **기능**: 현재 날짜, 시간, 온도, 습도를 OLED 디스플레이에 실시간으로 표시
* **동작**:
    * `Rotary Click`: 시간 설정을 위한 **편집 모드(EDIT_MODE)**로 진입

#### 🔵 편집 모드 (TIME_EDIT_MODE)
* **기능**: RTC(DS1302)의 시간을 사용자가 직접 수정 가능
* **제어 방식**:
    *  **CW (시계 방향 회전)**: 현재 항목의 값 **증가 (+1)**
    *  **CCW (반시계 방향 회전)**: 현재 항목의 값 **감소 (-1)** 
    *  **Click (버튼 클릭)**: 수정된 값을 저장하고 **다음 설정 항목으로 이동**

### 3. 설정 순서 (Edit Sequence)

편집 모드 진입 시 다음과 같은 순서로 상태가 순환

1.  **EDIT_YEAR**: 연도 설정
2.  **EDIT_MONTH**: 월 설정
3.  **EDIT_DAY**: 일 설정
4.  **EDIT_HOUR**: 시 설정
5.  **EDIT_MINUTE**: 분 설정
6.  **EDIT_SECOND**: 초 설정
    * *(마지막 단계에서 클릭 시 다시 **SCREEN_NORMAL** 상태로 복귀)*
---

## 🧵 어플리케이션 아키텍처: 멀티스레드 구조 (Multi-threaded Application)

 센서 데이터의 실시간성과 UI 반응성을 보장하기 위해 **POSIX Pthread**를 활용한 멀티스레드 구조로 설계되었음.

**생산자-소비자(Producer-Consumer) 패턴**을 적용하여, 각 하드웨어를 제어하는 독립적인 스레드들이 데이터를 수집하고, 공유 자원을 통해 OLED 디스플레이 스레드로 전달

### 1. 스레드 구조도 (Thread Architecture)

<img width="1781" height="1009" alt="image" src="https://github.com/user-attachments/assets/022ca5cc-3bae-430d-86c8-a883ec0c448c" />


### 2. 공유 자원 및 동기화 (Shared Resource & Synchronization)

* **구조체 (`struct Shared_data_t`)**: 모든 스레드가 접근하는 중앙 데이터 저장소, 현재 시간, 온습도 값, 사용자 입력 상태(Click, CW, CCW) 등을 저장
* **동기화 (Synchronization)**: 여러 스레드가 동시에 공유 메모리에 접근할 때 발생할 수 있는 데이터 경쟁을 방지하기 위해 Mutex를 사용하여 임계 구역을 보호


---

## 🎬 시연 영상
---


---
## 🚀 결과 및 배운 점 (Results & Lessons Learned)

본 프로젝트를 통해 하드웨어 제어부터 사용자 애플리케이션까지 이어지는 임베디드 리눅스 시스템의 전체 스택(Full-stack)을 직접 구현했다.

### 1. 모듈화된 드라이버 개발과 통합 (Modular Driver Development)
* **개별 모듈화 전략**: 거대한 하나의 코드를 작성하는 대신, 각 센서(OLED, DHT11, Rotary Encoder)별로 독립적인 **캐릭터 디바이스 드라이버(`*.ko`)**를 먼저 개발하고 검증했다.
* **유지보수성 향상**: 하드웨어 의존적인 코드를 커널 영역에 격리시킴으로써, 애플리케이션 코드는 하드웨어 변경에 영향을 받지 않는 유연한 구조를 확보했다.

### 2. 멀티스레드와 데이터 동기화 (Concurrency & Stability)
* **공유 자원 구조 설계**: 다수의 센서 데이터를 효율적으로 처리하기 위해 전역 공유 구조체(`struct Shared_data_t`)를 설계하여 데이터 흐름의 중심을 잡았다.
* **Producer-Consumer 패턴 적용**:
    * **Producer (Input Threads)**: 각 센서 스레드는 주기적으로 데이터를 읽어와 공유 구조체에 업데이트하는 역할에만 집중하도록 구현했다.
    * **Consumer (Output Thread)**: OLED 스레드는 데이터 수집 대기 시간 없이, 공유 구조체의 최신 값을 가져와 화면을 그리는 역할만 수행한다.
* **시스템 안정성 확보**: 이러한 역할 분리를 통해, 특정 센서의 응답 지연이 전체 UI의 반응 속도(Refresh Rate)를 저하시키지 않는 **Non-blocking 하고 부드러운 사용자 경험**을 구현했다.

### 3. 총평 (Conclusion)
단순한 센서 제어를 넘어, 리눅스 커널의 VFS 인터페이스를 준수하는 드라이버"와 "동시성을 고려한 멀티스레드 애플리케이션"을 유기적으로 결합해보며, 임베디드 소프트웨어 아키텍처의 중요성을 깊이 이해하는 계기가 되었다.

