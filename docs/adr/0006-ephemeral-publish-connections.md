# ADR-0006: 발행마다 단명 연결, 워커는 basic.get 폴링

상태: 채택

## 맥락

텔레메트리 발행은 저빈도다(metrics 기본 60s, inventory 3600s). AMQP 관용은 연결 재사용과 basic.consume 구독이지만, 장수명 연결은 stale 상태 관리(heartbeat, 재연결, 채널 예외 복구)를 늘 이고 가야 한다. 저빈도 워크로드에선 그 복잡성이 robustness를 오히려 해친다.

## 결정

- 발행(inventory/metrics/error): 발행마다 단명(ephemeral) 연결. connect -> login(SASL PLAIN) -> channel 1 -> confirm.select -> publish -> confirm -> close. exchange는 passive declare(존재 확인). 저빈도라 연결 오버헤드가 무의미하고 stale 상태 관리를 통째로 없앤다.
- 워커(task.install): persistent 연결 하나를 tick마다 재사용하고, 자기 큐(`agent.tasks.<agent_id>`)를 basic.get으로 폴링한다(basic.consume 아님). 큐는 엔진이 첫 task 때 lazy 생성하므로 워커는 큐 부재(404)를 에러가 아니라 "대기"로 구분한다.

## 결과

- 발행 경로는 상태가 없어 견고하다. 실패 시 백오프 무한 재시도(inventory/metrics) 후 복구 알림 발행, error는 1회.
- 워커는 채널 예외를 전체 reconnect 없이 채널 1만 재오픈해 복구하고, 브로커 다운 시 죽지 않고 백오프 재연결한다.
- 단일 프로세스 단일 루프가 metrics 발행과 worker tick을 같은 회전에서 처리한다(스레드 없음). 태스크 실행만 자식(Linux fork / Windows thread)으로 분리.
- 발행 빈도가 크게 오르면 재고 대상이다.
