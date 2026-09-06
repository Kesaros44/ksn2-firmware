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
 *      b. 단어 삭제 조합 전송 - Windows(param1=0)는 Ctrl+Backspace,
 *         macOS(param1=1)는 Option+Backspace.
 *      c. 한/영 전환(LANG1) 전송.
 *      d. 복사해둔 버퍼를 그대로 순서대로 재입력.
 *
 * 전부 zmk_behavior_queue_add()로 큐에 넣는다 - ZMK의 매크로 behavior가
 * 쓰는 것과 동일한 공식 비동기 큐 API라서, 이 파일이 어떤 스레드
 * 컨텍스트에서 호출되든 블로킹 없이 안전하게 순서대로 처리된다.
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

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define WORD_FLIP_MAX_LEN 24
#define WORD_FLIP_TAP_MS 40
#define WORD_FLIP_WAIT_MS 30

struct word_flip_key {
    uint32_t keycode;
    uint8_t explicit_modifiers;
};

static struct word_flip_key buffer[WORD_FLIP_MAX_LEN];
static size_t buffer_len;

static bool is_letter(uint32_t keycode) {
    return keycode >= A && keycode <= Z;
}

static int word_flip_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE; /* release는 무시, press만 본다 */
    }

    if (is_letter(ev->keycode)) {
        if (buffer_len < WORD_FLIP_MAX_LEN) {
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

static void queue_kp(struct zmk_behavior_binding_event *event, uint32_t param1) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = "KP",
        .param1 = param1,
        .param2 = 0,
    };
    zmk_behavior_queue_add(event, binding, true, WORD_FLIP_TAP_MS);
    zmk_behavior_queue_add(event, binding, false, WORD_FLIP_WAIT_MS);
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

    /* param1: 0 = Windows(Ctrl+Backspace), 1 = macOS(Option+Backspace) */
    uint32_t delete_word = binding->param1 == 1 ? LA(BSPC) : LC(BSPC);

    queue_kp(&event, delete_word);
    queue_kp(&event, LANG1);

    for (size_t i = 0; i < snapshot_len; i++) {
        uint32_t param1 = ((uint32_t)snapshot[i].explicit_modifiers << 24) | snapshot[i].keycode;
        queue_kp(&event, param1);
    }

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
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, word_flip_init, NULL, NULL, NULL, POST_KERNEL,
                         CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &word_flip_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
