target_sources(app PRIVATE src/ksn2_conn_status.c)

# 한/영 오타 자동 되돌리기 트리거 behavior (&word_flip) - config/src/ksn2_word_flip.c
# 참고. DT_HAS_COMPAT_STATUS_OKAY로 자체 게이팅되므로 devicetree에 노드가
# 없는 빌드(예: settings_reset)에서는 그냥 빈 번역 단위가 됨.
target_sources(app PRIVATE src/ksn2_word_flip.c)
