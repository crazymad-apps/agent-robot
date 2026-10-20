#pragma once
#include <stdio.h>
#define ESP_LOGW(tag, ...) do { (void)(tag); printf(__VA_ARGS__); puts(""); } while (0)
