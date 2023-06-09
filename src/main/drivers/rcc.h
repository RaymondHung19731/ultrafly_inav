#pragma once

#include "rcc_types.h"
#include "rcc_at32f43x_periph.h"

enum rcc_reg {
    RCC_EMPTY = 0,   // make sure that default value (0) does not enable anything
    RCC_AHB,        //0x20      
    RCC_APB2,       //0x40  CRM->apb2en
    RCC_APB1,       //0x60  CRM->apb1en end AT32 
    RCC_AHB1,       //0x80 
    RCC_AHB2,
    RCC_APB1L,
    RCC_APB1H,
    RCC_AHB3,
    RCC_APB3,
    RCC_AHB4,
    RCC_APB4
};
 
#define RCC_ENCODE(reg, mask) (((reg) << 5) | LOG2_32BIT(mask))

#ifdef AT32F43x

#define RCC_AHB1(periph) RCC_ENCODE(RCC_AHB1,   CRM_AHB1_ ## periph ## _PER_MASK)
#define RCC_AHB2(periph) RCC_ENCODE(RCC_AHB2,   CRM_AHB2_ ## periph ## _PER_MASK)
#define RCC_AHB3(periph) RCC_ENCODE(RCC_AHB3,   CRM_AHB3_ ## periph ## _PER_MASK)
#define RCC_APB1(periph) RCC_ENCODE(RCC_APB1, 	CRM_APB1_ ## periph ## _PER_MASK)
#define RCC_APB2(periph) RCC_ENCODE(RCC_APB2, 	CRM_APB2_ ## periph ## _PER_MASK)

#else
#define RCC_AHB(periph) RCC_ENCODE(RCC_AHB, RCC_AHBENR_ ## periph ## EN)
#define RCC_AHB1(periph) RCC_ENCODE(RCC_AHB1, RCC_AHB1ENR_ ## periph ## EN)
#define RCC_APB1(periph) RCC_ENCODE(RCC_APB1, RCC_APB1ENR_ ## periph ## EN)
#define RCC_APB2(periph) RCC_ENCODE(RCC_APB2, RCC_APB2ENR_ ## periph ## EN)
#endif

#ifdef STM32H7
#define RCC_AHB2(periph) RCC_ENCODE(RCC_AHB2, RCC_AHB2ENR_ ## periph ## EN)
#define RCC_AHB3(periph) RCC_ENCODE(RCC_AHB3, RCC_AHB3ENR_ ## periph ## EN)
#define RCC_AHB4(periph) RCC_ENCODE(RCC_AHB4, RCC_AHB4ENR_ ## periph ## EN)
#define RCC_APB1L(periph) RCC_ENCODE(RCC_APB1L, RCC_APB1LENR_ ## periph ## EN)
#define RCC_APB1H(periph) RCC_ENCODE(RCC_APB1H, RCC_APB1HENR_ ## periph ## EN)
#define RCC_APB3(periph) RCC_ENCODE(RCC_APB3, RCC_APB3ENR_ ## periph ## EN)
#define RCC_APB4(periph) RCC_ENCODE(RCC_APB4, RCC_APB4ENR_ ## periph ## EN)
#endif

void RCC_ClockCmd(rccPeriphTag_t periphTag, FunctionalState NewState);
void RCC_ResetCmd(rccPeriphTag_t periphTag, FunctionalState NewState);

