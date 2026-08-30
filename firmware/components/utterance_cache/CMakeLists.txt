idf_component_register(
    SRCS "utterance_cache.c" "utterance_cache_memory.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES device_protocol heap
)
