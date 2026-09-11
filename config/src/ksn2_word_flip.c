/*
 * "단어 되돌리기" (word-flip) 트리거 behavior.
 *
 * 목적: 방금 입력한 단어가 한/영 오타(다른 언어 모드로 잘못 입력됨)일 때,
 * 사용자가 직접 판단하고 트리거 키를 눌러 그 단어를 지우고 반대 언어
 * 모드로 재입력한다. 호스트의 현재 IME 상태를 알 필요가 없다 - 무조건
 * "반대로 뒤집기"만 하므로 상태 추정이 틀릴 일이 없다 (사람이 트리거
 * 시점을 판단하기 때문).
 *
 * KSN-1의 ksn1_word_flip.c와 동일한 구현 (central이 왼쪽이라는 점만
 * 다를 뿐 이 파일 자체는 central/peripheral 어느 쪽에 컴파일돼도 무방함 -
 * zmk_keycode_state_changed는 어차피 central에서만 실제 키 이벤트로
 * 발생함).
 *
 * 동작:
 *   1. 이 파일의 리스너가 모든 A-Z 키 입력을 최근 순서대로 버퍼에 담아둔다.
 *      스페이스/엔터/탭/그 외 A-Z가 아닌 키가 눌리면 버퍼를 비운다(단어
 *      경계).
 *   2. 트리거 키(&word_flip <os>)를 누르면:
 *      a. 현재 버퍼를 로컬로 복사하고 전역 버퍼를 즉시 비운다.
 *      b. 한/영 전환 전송 - Windows(param1=0)는 LANG1, macOS(param1=1)는
 *         Caps Lock. 삭제보다 먼저 보내야 한다(아래 on_word_flip_binding_
 *         pressed의 순서 주석 참고).
 *      c. 단어 삭제 조합 전송 - Windows는 Ctrl+Backspace, macOS는
 *         Option+Backspace.
 *      d. 복사해둔 버퍼를 그대로 순서대로 재입력.
 *
 * 전부 zmk_behavior_queue_add()로 큐에 넣는다 - ZMK의 매크로 behavior가
 * 쓰는 것과 동일한 공식 비동기 큐 API라서, 이 파일이 어떤 스레드
 * 컨텍스트에서 호출되든 블로킹 없이 안전하게 순서대로 처리된다.
 *
 * 수정 이력(2026-09-11): "한영자동변환키를 누르면 실제로 입력했던 단어와
 * 무관하게 항상 같은 엉뚱한 문자열이 재입력되는" 버그를 다음 두 가지로
 * 확인/수정함.
 *
 * [버그 1 - 근본 원인] 자기 자신의 재입력을 다시 캡처하는 무한 피드백:
 *   위 2-d 단계에서 큐에 넣은 재입력 자체도 &kp(key_press) behavior를
 *   거쳐 zmk_keycode_state_changed 이벤트를 "다시" 발생시킨다. 이 파일의
 *   word_flip_keycode_listener는 그 이벤트 출처를 구분하지 않고 시스템의
 *   모든 zmk_keycode_state_changed를 구독하므로, 자신이 방금 재입력한
 *   글자들을 "사용자가 새로 타이핑한 단어"로 다시 buffer에 채워 넣었다.
 *   그 결과 실제로 어떤 단어를 쳤든 상관없이 buffer 내용이 이전 트리거의
 *   재입력 결과로 계속 오염/고착되어, 다음 트리거를 누르면 방금 전
 *   되돌리기의 잔재가 반복 재생되는 것처럼 보이는 문제가 발생했다.
 *   -> is_replaying 플래그로 재입력 구간 동안 리스너를 비활성화해서
 *      해결(재입력 시퀀스 전체 소요 시간만큼 지연 후 자동 해제).
 *
 * [버그 2 - 방어적 수정] 재입력 시 HID usage page 누락:
 *   버퍼에는 ev->keycode(=page가 빠진 순수 usage ID)만 저장돼 있었고,
 *   재입력 시 이를 페이지 없이 그대로 &kp param1에 실었다. ZMK가 인코딩된
 *   usage의 page 필드가 0이면 HID_USAGE_KEY로 대체 해석해주는 폴백이
 *   있어 당장 동작은 하지만, 향후 ZMK 버전에서 이 폴백이 사라지면 조용히
 *   깨지는 잠재적 문제였다. buffer에 usage_page도 함께 저장하고 재입력
 *   시 ZMK_HID_USAGE(page, id)로 완전한 usage를 명시적으로 재구성하도록
 *   수정해 폴백에 의존하지 않게 했다.
 *
 * 주의(2026-09-06): 이 세션에 west 빌드 툴체인이 없어 실제 컴파일 검증을
 * 못 했음. ZMK 공식 문서와 ksn1-firmware에 이미 있는 검증된 코드
 * (calc_macro, ksn1_conn_status_relay 등)의 실제 API를 최대한 그대로
 * 재사용했지만, GitHub Actions 빌드 결과를 반드시 먼저 확인할 것.
 */

#define DT_DRV_COMPAT ksn_behavior_word_flip

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if (!defined(CONFIG_ZMK_SPLIT) || defined(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) && \
    DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define WORD_FLIP_MAX_LEN 24
#define WORD_FLIP_TAP_MS 40
#define WORD_FLIP_WAIT_MS 30
/* macOS는 한/영 전환에 Caps Lock을 쓰는데(시스템 설정 "Caps Lock 키로 ABC
 * 입력 소스 전환"), 이 전환은 즉시 반영되지 않고 수백 ms 지연이 있는 것으로
 * 잘 알려져 있다. Windows의 LANG1처럼 30ms 뒤에 바로 다음 키를 보내면 아직
 * 이전 입력 모드인 상태에서 삭제/재입력이 들어가 버린다. 그래서 맥에서는
 * 전환 키 뒤에만 넉넉히 기다린다. */
#define WORD_FLIP_MAC_TOGGLE_WAIT_MS 350
/* is_replaying 플래그를 해제하기까지, 실제 큐 처리 시간에 더해 주는
 * 안전 여유분. 큐 처리 자체가 살짝 밀리더라도 재입력 이벤트가 새 단어로
 * 오인되지 않도록 넉넉히 잡는다. */
#define WORD_FLIP_REPLAY_GUARD_SLACK_MS 60

struct word_flip_key {
    uint16_t usage_page;
    uint32_t keycode;
    uint8_t explicit_modifiers;
};

static struct word_flip_key buffer[WORD_FLIP_MAX_LEN];
static size_t buffer_len;

/* 재입력(replay) 구간 동안 true. 이 사이에 들어오는 zmk_keycode_state_changed는
 * 우리가 방금 큐에 넣은 재입력 자신이 발생시킨 것이므로 캡처 리스너가
 * 무시해야 한다 (버그 1 참고). */
static bool is_replaying = false;
static struct k_work_delayable replay_guard_work;

static void replay_guard_expire(struct k_work *work) {
    is_replaying = false;
}

/* 주의: 이벤트의 ev->keycode는 usage page가 빠진 순수 usage ID(A=0x04 ...
 * Z=0x1D)다. 반면 keys.h의 A/Z 매크로는 ZMK_HID_USAGE(page, id)로 page가
 * 상위 비트에 붙은 값(A=0x70004)이라 그대로 비교하면 절대 참이 되지 않는다.
 * page는 ev->usage_page로 따로 확인하고, 범위 비교는 ZMK_HID_USAGE_ID()로
 * 벗겨낸 ID끼리 해야 한다. */
static bool is_letter(uint16_t usage_page, uint32_t keycode) {
    return usage_page == HID_USAGE_KEY && keycode >= ZMK_HID_USAGE_ID(A) &&
           keycode <= ZMK_HID_USAGE_ID(Z);
}

static int word_flip_keycode_listener(const zmk_event_t *eh) {
    if (is_replaying) {
        /* 우리 자신이 재입력 중인 키 이벤트 - 새 단어로 캡처하면 안 됨
         * (버그 1). 단어 경계 리셋도 이 구간에서는 하지 않는다: 재입력
         * 중간에 섞인 비-문자 키(전환/삭제)에 의해 방금 복사해둔 snapshot
         * 재생이 끝나기도 전에 buffer_len이 건드려질 이유가 없다. */
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE; /* release는 무시, press만 본다 */
    }

    if (is_letter(ev->usage_page, ev->keycode)) {
        if (buffer_len < WORD_FLIP_MAX_LEN) {
            buffer[buffer_len].usage_page = ev->usage_page;
            buffer[buffer_len].keycode = ev->keycode;
            buffer[buffer_len].explicit_modifiers = ev->explicit_modifiers;
            buffer_len++;
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* A-Z가 아닌 키(스페이스/엔터/백스페이스/화살표 등) = 단어 경계 */
    buffer_len = 0;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(word_flip_capture, word_flip_keycode_listener);
ZMK_SUBSCRIPTION(word_flip_capture, zmk_keycode_state_changed);

static uint32_t queue_kp_ex(struct zmk_behavior_binding_event *event, uint32_t param1,
                             uint32_t post_wait_ms) {
    struct zmk_behavior_binding binding = {
        /* "KP"는 devicetree 라벨일 뿐이고, zmk_behavior_get_binding()이 찾는
         * 디바이스 이름은 노드 이름인 "key_press"다 (ZMK app/dts/behaviors/
         * key_press.dtsi의 `kp: key_press { ... }`). "KP"로 두면 조회가
         * NULL이 되어 "No behavior assigned" 경고만 남기고 아무것도 안 나간다. */
        .behavior_dev = "key_press",
        .param1 = param1,
        .param2 = 0,
    };
    zmk_behavior_queue_add(event, binding, true, WORD_FLIP_TAP_MS);
    zmk_behavior_queue_add(event, binding, false, post_wait_ms);
    return WORD_FLIP_TAP_MS + post_wait_ms;
}

static uint32_t queue_kp(struct zmk_behavior_binding_event *event, uint32_t param1) {
    return queue_kp_ex(event, param1, WORD_FLIP_WAIT_MS);
}

static int on_word_flip_binding_pressed(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    struct word_flip_key snapshot[WORD_FLIP_MAX_LEN];
    size_t snapshot_len = buffer_len;

    if (snapshot_len == 0) {
        return ZMK_BEHAVIOR_OPAQUE; /* 되돌릴 게 없으면 아무것도 안 함 */
    }

    memcpy(snapshot, buffer, sizeof(struct word_flip_key) * snapshot_len);
    /* 아래에서 보낼 백스페이스가 이 파일의 리스너에도 잡혀서 버퍼를 지울
     * 것이므로, 재입력에 쓸 내용은 이미 snapshot에 복사해뒀으니 미리 비움 */
    buffer_len = 0;

    /* 지금부터 큐에 넣는 모든 키 입력(전환/삭제/재입력)은 이 behavior
     * 자신이 발생시키는 것이므로, 그 사이에 캡처 리스너가 다시 buffer에
     * 채워 넣지 못하게 막는다 (버그 1). 실제 처리 시간을 총 wait_ms로
     * 누적해서 그 시점 이후에 자동 해제한다. */
    is_replaying = true;
    uint32_t total_wait_ms = 0;

    /* param1: 0 = Windows, 1 = macOS
     *
     * 단어 삭제:  Windows = Ctrl+Backspace,  macOS = Option+Backspace
     * 한/영 전환: Windows = LANG1(HID 0x90, 전용 한/영 키의 표준 코드)
     *             macOS   = Caps Lock
     *
     * macOS는 LANG1을 한/영 전환으로 처리하지 않는다 - 그 코드는 일본어 JIS
     * 가나 키(kVK_JIS_Kana)로 해석된다. Apple이 공식적으로 안내하는 전환
     * 방법은 Control+Space / Control+Option+Space / Caps Lock / Fn(지구본)
     * 뿐이고, 이 키보드 사용자는 Caps Lock 방식을 쓰므로 그쪽에 맞춘다.
     * (시스템 설정 > 키보드 > 입력 소스에서 "Caps Lock 키로 ABC 입력 소스
     * 전환"이 켜져 있어야 한다. 짧게 누르면 입력 소스 전환, 길게 누르면
     * 실제 Caps Lock이므로 여기서 보내는 40ms 탭은 전환으로 동작한다.) */
    bool is_mac = binding->param1 == 1;
    uint32_t delete_word = is_mac ? LA(BSPC) : LC(BSPC);
    uint32_t lang_toggle = is_mac ? CLCK : LANG1;

    /* 순서 주의: 한/영 전환을 반드시 먼저 보낸다. 한글 입력 중이면 마지막
     * 음절이 IME의 조합(composition) 상태로 물려 있어서, 이때 오는
     * Ctrl/Option+Backspace는 앱까지 가지 않고 IME가 가로채 조합 중인 음절만
     * 지운다(앞쪽 한글이 남는 증상). 전환 키를 먼저 보내면 그 시점에 조합이
     * 확정되고 IME가 빠지므로 뒤따르는 단어 삭제가 단어 전체에 적용된다. */
    total_wait_ms += queue_kp_ex(&event, lang_toggle,
                                  is_mac ? WORD_FLIP_MAC_TOGGLE_WAIT_MS : WORD_FLIP_WAIT_MS);
    total_wait_ms += queue_kp(&event, delete_word);

    for (size_t i = 0; i < snapshot_len; i++) {
        /* 버그 2 수정: page가 빠진 raw id를 그대로 싣지 않고, 캡처해둔
         * usage_page로 완전한 HID usage를 재구성해서 보낸다. */
        uint32_t full_usage = ZMK_HID_USAGE(snapshot[i].usage_page, snapshot[i].keycode);
        uint32_t param1 = ((uint32_t)snapshot[i].explicit_modifiers << 24) | full_usage;
        total_wait_ms += queue_kp(&event, param1);
    }

    k_work_reschedule(&replay_guard_work,
                       K_MSEC(total_wait_ms + WORD_FLIP_REPLAY_GUARD_SLACK_MS));

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_word_flip_binding_released(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api word_flip_driver_api = {
    .binding_pressed = on_word_flip_binding_pressed,
    .binding_released = on_word_flip_binding_released,
};

static int word_flip_init(const struct device *dev) {
    k_work_init_delayable(&replay_guard_work, replay_guard_expire);
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, word_flip_init, NULL, NULL, NULL, POST_KERNEL,
                         CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &word_flip_driver_api);

#endif /* central-only && DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
