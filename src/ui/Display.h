#pragma once
#include <bitset>
#include <STM32FreeRTOS.h>
#include "../SysState.h"

void displayUpdateTask(void* pvParameters);
void initialiseDisplay();