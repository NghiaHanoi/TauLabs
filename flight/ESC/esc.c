/**
 ******************************************************************************
 * @addtogroup ESC esc
 * @brief The main ESC code
 *
 * @file       esc.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      INSGPS Test Program
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* OpenPilot Includes */
#include "pios.h"
#include "esc_fsm.h"
#include "fifo_buffer.h"
#include <pios_stm32.h>

//TODO: Check the ADC buffer pointer and make sure it isn't dropping swaps
//TODO: Check the time commutation is being scheduled, make sure it's the future
//TODO: Slave two timers together so in phase
//TODO: Ideally lock ADC and delay timers together to both
//TODO: Look into using TIM1
//know the exact time of each sample and the PWM phase

//#define BACKBUFFER_ZCD
//#define BACKBUFFER_ADC
//#define BACKBUFFER_DIFF

/* Prototype of PIOS_Board_Init() function */
extern void PIOS_Board_Init(void);

#define DOWNSAMPLING 1

#if defined(BACKBUFFER_ADC) || defined(BACKBUFFER_ZCD) || defined(BACKBUFFER_DIFF)
uint16_t back_buf[8096];
uint16_t back_buf_point = 0;
#endif

#define LED_ERR LED1
#define LED_GO  LED2
#define LED_MSG LED3

int16_t zero_current = 0;

volatile uint8_t low_pin;
volatile uint8_t high_pin;
volatile uint8_t undriven_pin;
volatile bool pos;

const uint8_t dT = 1e6 / PIOS_ADC_RATE; // 6 uS per sample at 160k
float rate = 0;

static void test_esc();
static void panic(int diagnostic_code);
uint16_t pwm_duration ;
uint32_t counter = 0;

#define NUM_SETTLING_TIMES 20
uint32_t timer;
uint16_t timer_lower;
uint32_t step_period = 0x0080000;
uint32_t last_step = 0;
int16_t low_voltages[3];
int32_t avg_low_voltage;
struct esc_fsm_data * esc_data = 0;

/**
 * @brief ESC Main function
 */
uint16_t input;

int main()
{
	esc_data = 0;
	PIOS_Board_Init();

	PIOS_ADC_Config(DOWNSAMPLING);
	
	// TODO: Move this into a PIOS_DELAY function
	TIM_OCInitTypeDef tim_oc_init = {
		.TIM_OCMode = TIM_OCMode_PWM1,
		.TIM_OutputState = TIM_OutputState_Enable,
		.TIM_OutputNState = TIM_OutputNState_Disable,
		.TIM_Pulse = 0,
		.TIM_OCPolarity = TIM_OCPolarity_High,
		.TIM_OCNPolarity = TIM_OCPolarity_High,
		.TIM_OCIdleState = TIM_OCIdleState_Reset,
		.TIM_OCNIdleState = TIM_OCNIdleState_Reset,
	};
	TIM_OC1Init(TIM4, &tim_oc_init);
	TIM_ITConfig(TIM4, TIM_IT_CC1, ENABLE);  // Enabled by FSM

	NVIC_InitTypeDef NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannel = TIM4_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_HIGH;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

	// This pull up all the ADC voltages so the BEMF when at -0.7V
	// is still positive
	PIOS_GPIO_Enable(0);
	PIOS_GPIO_Enable(1);
	PIOS_GPIO_Off(0);

	PIOS_LED_Off(LED_ERR);
	PIOS_LED_On(LED_GO);
	PIOS_LED_On(LED_MSG);

	test_esc();

	esc_data = esc_fsm_init();
	esc_data->speed_setpoint = 0;

	PIOS_ADC_StartDma();
	
	extern uint32_t pios_rcvr_group_map[];
	while(1) {
		counter++;

		input = PIOS_RCVR_Read(pios_rcvr_group_map[0],1);
		esc_data->speed_setpoint = (input < 1050) ? 0 : 400 + ((input - 1050) << 3);

		esc_process_static_fsm_rxn();
	}
	return 0;
}

// When driving both legs to ground mid point is 580
#define MAX_RUNNING_FILTER 64
uint32_t DEMAG_BLANKING = 100;

#define MAX_CURRENT_FILTER 64
int32_t current_filter[MAX_CURRENT_FILTER];
int32_t current_filter_sum = 0;
int32_t current_filter_pointer = 0;

static int16_t diff_filter[MAX_RUNNING_FILTER];
static int32_t diff_filter_pointer = 0;
static int32_t running_filter_length = 16;
static int32_t running_filter_sum = 0;

uint32_t samples_averaged;

uint32_t calls_to_detect = 0;
uint32_t calls_to_last_detect = 0;

int32_t low_threshold = -0;
int32_t high_threshold = 0;

#include "pios_adc_priv.h"
uint32_t detected;
uint32_t bad_flips;
void DMA1_Channel1_IRQHandler(void)
{	
	static bool negative = false;
	static enum pios_esc_state prev_state = ESC_STATE_AB;
	static int16_t * raw_buf;
	enum pios_esc_state curr_state;

	if (DMA_GetFlagStatus(pios_adc_devs[0].cfg->full_flag /*DMA1_IT_TC1*/)) {	// whole double buffer filled
		pios_adc_devs[0].valid_data_buffer = &pios_adc_devs[0].raw_data_buffer[pios_adc_devs[0].dma_half_buffer_size];
		DMA_ClearFlag(pios_adc_devs[0].cfg->full_flag);
//		PIOS_GPIO_On(1);		
	}
	else if (DMA_GetFlagStatus(pios_adc_devs[0].cfg->half_flag /*DMA1_IT_HT1*/)) {
		pios_adc_devs[0].valid_data_buffer = &pios_adc_devs[0].raw_data_buffer[0];
		DMA_ClearFlag(pios_adc_devs[0].cfg->half_flag);
//		PIOS_GPIO_Off(1); 
	}
	else {
		// This should not happen, probably due to transfer errors
		DMA_ClearFlag(pios_adc_devs[0].cfg->dma.irq.flags /*DMA1_FLAG_GL1*/);
		return;
	}

	bad_flips += (raw_buf == pios_adc_devs[0].valid_data_buffer);
	raw_buf = (int16_t *) pios_adc_devs[0].valid_data_buffer;

	curr_state = PIOS_ESC_GetState();

#ifdef BACKBUFFER_ADC
	// Debugging code - keep a buffer of old ADC values
	back_buf[back_buf_point++] = raw_buf[0];
	back_buf[back_buf_point++] = raw_buf[1];
	back_buf[back_buf_point++] = raw_buf[2];
	back_buf[back_buf_point++] = raw_buf[ 3];
	if(back_buf_point >= (NELEMENTS(back_buf)-3))
#endif

	if((PIOS_DELAY_DiffuS(esc_data->last_swap_time) < DEMAG_BLANKING) ||
	   esc_data->detected)
		return;
	
//	running_filter_length = (esc_data->current_speed > 4000) ? 4 :
//		(esc_data->current_speed > 2000) ? 4 : 16;

	// Smooth the estimate of current a bit 	
	int32_t this_current = raw_buf[0] - zero_current;
	current_filter_sum += this_current - current_filter[current_filter_pointer];
	current_filter[current_filter_pointer] = this_current;
	current_filter_pointer++;
	if(current_filter_pointer >= MAX_CURRENT_FILTER)
		current_filter_pointer = 0;
	esc_data->current = current_filter_sum / MAX_CURRENT_FILTER;

	// If detected this commutation don't bother here
	if(curr_state != prev_state) {
		for(uint8_t j = 0; j < MAX_RUNNING_FILTER; j++)
			diff_filter[j] = 0;

		prev_state = curr_state;
		calls_to_detect = 0;
		negative = false;
		running_filter_sum = 0;
		diff_filter_pointer = 0;
		samples_averaged = 0;
		
		switch(curr_state) {
			case ESC_STATE_AC:
				undriven_pin = 1;
				pos = true;
				break;
			case ESC_STATE_CA:
				undriven_pin = 1;
				pos = false;
				break;
			case ESC_STATE_AB:
				undriven_pin = 2;
				pos = false;
				break;
			case ESC_STATE_BA:
				undriven_pin = 2;
				pos = true;
				break;
			case ESC_STATE_BC:
				undriven_pin = 0;
				pos = false;
				break;
			case ESC_STATE_CB:
				undriven_pin = 0;
				pos = true;
				break;
			default:
				PIOS_ESC_Off();
		}
	}

	calls_to_detect++;

	// Doesn't work quite right yet
	int32_t undriven = raw_buf[1 + undriven_pin]; // - low_voltages[undriven_pin];
	int32_t ref = (raw_buf[1] + raw_buf[2] + raw_buf[3]) / 3; // - avg_low_voltage) / 3;
	int32_t diff = pos ? undriven - ref : ref - undriven;
	
	// Update running sum and history
	running_filter_sum += diff - diff_filter[diff_filter_pointer];
	diff_filter[diff_filter_pointer] = diff;
	diff_filter_pointer++;
	if(diff_filter_pointer >= running_filter_length)
		diff_filter_pointer = 0;
	samples_averaged++;

	if(samples_averaged > running_filter_length) {
		//threshold = -hysteresis * running_filter_length;
		if(running_filter_sum < -low_threshold)
			negative = true;
		else if(running_filter_sum > high_threshold && negative) {
			detected++;
			esc_fsm_inject_event(ESC_EVENT_ZCD, 0);
			calls_to_last_detect = calls_to_detect;
			PIOS_GPIO_Toggle(1);
#ifdef BACKBUFFER_ZCD
			back_buf[back_buf_point++] = below_time;
			back_buf[back_buf_point++] = esc_data->speed_setpoint;
			if(back_buf_point > (sizeof(back_buf) / sizeof(back_buf[0])))
				back_buf_point = 0;
#endif /* BACKBUFFER_ZCD */
		}
	}
	
#ifdef BACKBUFFER_DIFF
	// Debugging code - keep a buffer of old ADC values
	back_buf[back_buf_point++] = diff;
	if(back_buf_point >= NELEMENTS(back_buf))
		back_buf_point = 0;
#endif
	
}

/* INS functions */
void panic(int diagnostic_code)
{
	PIOS_ESC_Off();
	// Polarity backwards
	PIOS_LED_On(LED_ERR);
	while(1) {
		for(int i=0; i<diagnostic_code; i++)
		{
			PIOS_LED_Toggle(LED_ERR);
			PIOS_DELAY_WaitmS(250);
			PIOS_LED_Toggle(LED_ERR);
			PIOS_DELAY_WaitmS(250);
		}
		PIOS_DELAY_WaitmS(1000);
	}
}

//TODO: Abstract out constants.  Need to know battery voltage too
void test_esc() {
	int32_t voltages[6][3];


	PIOS_ESC_Off();
	PIOS_DELAY_WaitmS(150);
	zero_current = PIOS_ADC_PinGet(0);

	PIOS_ESC_Arm();

	PIOS_ESC_TestGate(ESC_A_LOW);
	PIOS_DELAY_WaituS(250);
	low_voltages[0] = PIOS_ADC_PinGet(1);
	PIOS_ESC_TestGate(ESC_B_LOW);
	PIOS_DELAY_WaituS(250);
	low_voltages[1] = PIOS_ADC_PinGet(2);
	PIOS_ESC_TestGate(ESC_C_LOW);
	PIOS_DELAY_WaituS(250);
	low_voltages[2] = PIOS_ADC_PinGet(3);
	avg_low_voltage = low_voltages[0] + low_voltages[1] + low_voltages[2];

	PIOS_ESC_SetDutyCycle(0.5);
	PIOS_ESC_TestGate(ESC_A_LOW);
	PIOS_DELAY_WaituS(250);
	PIOS_ESC_SetDutyCycle(1);
	PIOS_DELAY_WaituS(100);
	voltages[1][0] = PIOS_ADC_PinGet(1);
	voltages[1][1] = PIOS_ADC_PinGet(2);
	voltages[1][2] = PIOS_ADC_PinGet(3);

	PIOS_ESC_SetDutyCycle(0.5);
	PIOS_ESC_TestGate(ESC_A_HIGH);
	PIOS_DELAY_WaituS(250);
	PIOS_ESC_SetDutyCycle(1);
	PIOS_DELAY_WaituS(100);
	voltages[0][0] = PIOS_ADC_PinGet(1);
	voltages[0][1] = PIOS_ADC_PinGet(2);
	voltages[0][2] = PIOS_ADC_PinGet(3);

	PIOS_ESC_SetDutyCycle(0.5);
	PIOS_ESC_TestGate(ESC_B_LOW);
	PIOS_DELAY_WaituS(250);
	PIOS_ESC_SetDutyCycle(1);
	PIOS_DELAY_WaituS(100);
	voltages[3][0] = PIOS_ADC_PinGet(1);
	voltages[3][1] = PIOS_ADC_PinGet(2);
	voltages[3][2] = PIOS_ADC_PinGet(3);

	PIOS_ESC_SetDutyCycle(0.5);
	PIOS_ESC_TestGate(ESC_B_HIGH);
	PIOS_DELAY_WaituS(250);
	PIOS_ESC_SetDutyCycle(1);
	PIOS_DELAY_WaituS(100);
	voltages[2][0] = PIOS_ADC_PinGet(1);
	voltages[2][1] = PIOS_ADC_PinGet(2);
	voltages[2][2] = PIOS_ADC_PinGet(3);

	PIOS_ESC_SetDutyCycle(0.5);
	PIOS_ESC_TestGate(ESC_C_LOW);
	PIOS_DELAY_WaituS(250);
	PIOS_ESC_SetDutyCycle(1);
	PIOS_DELAY_WaituS(100);
	voltages[5][0] = PIOS_ADC_PinGet(1);
	voltages[5][1] = PIOS_ADC_PinGet(2);
	voltages[5][2] = PIOS_ADC_PinGet(3);

	PIOS_ESC_SetDutyCycle(0.5);
	PIOS_ESC_TestGate(ESC_C_HIGH);
	PIOS_DELAY_WaituS(250);
	PIOS_ESC_SetDutyCycle(1);
	PIOS_DELAY_WaituS(100);
	voltages[4][0] = PIOS_ADC_PinGet(1);
	voltages[4][1] = PIOS_ADC_PinGet(2);
	voltages[4][2] = PIOS_ADC_PinGet(3);

	PIOS_ESC_Off();
	// If the particular phase isn't moving fet is dead
	if(voltages[0][0] < 1000)
		panic(1);
	if(voltages[1][0] > 700)
		panic(2);
	if(voltages[2][1] < 1000)
		panic(2);
	if(voltages[3][1] > 700)
		panic(3);
	if(voltages[4][2] < 1000)
		panic(4);
	if(voltages[5][2] > 700)
		panic(5);

	// TODO: If other channels don't follow then motor lead bad
}

void PIOS_TIM_4_irq_override();
extern void PIOS_DELAY_timeout();
void TIM4_IRQHandler(void) __attribute__ ((alias ("PIOS_TIM_4_irq_handler")));
static void PIOS_TIM_4_irq_handler (void)
{
	if(TIM_GetITStatus(TIM4,TIM_IT_CC1))
		PIOS_DELAY_timeout();
	else
		PIOS_TIM_4_irq_override();
}

/*
 Notes:
 1. For start up, definitely want to use complimentary PWM to ground the lower side, making zero crossing truly "zero"
 2. May want to use the "middle" sensor to actually pull it up, so that zero is above zero (in ADC range).  Should still
    see BEMF at -0.7 (capped by transistor range) relative to that point (divided down by whatever)
 3. Possibly use an inadequate voltage divider plus use the TVS cap to keep the part of the signal near zero clean
 */


