idf_component_register(
    SRCS "terminal_interaction.c" "terminal_interaction_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES board esp_driver_gpio esp_timer
)
