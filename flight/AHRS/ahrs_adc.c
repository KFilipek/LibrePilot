/**
 ******************************************************************************
 * @addtogroup AHRS AHRS
 * @brief The AHRS Modules perform
 *
 * @{ 
 * @addtogroup AHRS_ADC AHRS ADC
 * @brief Specialized ADC code for double buffered DMA for AHRS
 * @{ 
 *
 *
 * @file       ahrs.c
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

#include "ahrs_adc.h"

// Remap the ADC DMA handler to this one
void DMA1_Channel1_IRQHandler()
    __attribute__ ((alias("AHRS_ADC_DMA_Handler")));

//! Where the raw data is stored
volatile int16_t raw_data_buffer[MAX_SAMPLES];	// Double buffer that DMA just used

//! Various configuration settings
struct {
	volatile int16_t *valid_data_buffer;
	volatile uint8_t adc_oversample;
	int16_t fir_coeffs[MAX_OVERSAMPLING];
} adc_config;

//! Filter coefficients used in decimation.  Limited order so filter can't run between samples
float downsampled_buffer[PIOS_ADC_NUM_PINS];

static ADCCallback callback_function = (ADCCallback) NULL;

/* Local Variables */
static GPIO_TypeDef *ADC_GPIO_PORT[PIOS_ADC_NUM_PINS] = PIOS_ADC_PORTS;
static const uint32_t ADC_GPIO_PIN[PIOS_ADC_NUM_PINS] = PIOS_ADC_PINS;
static const uint32_t ADC_CHANNEL[PIOS_ADC_NUM_PINS] = PIOS_ADC_CHANNELS;

static ADC_TypeDef *ADC_MAPPING[PIOS_ADC_NUM_PINS] = PIOS_ADC_MAPPING;
static const uint32_t ADC_CHANNEL_MAPPING[PIOS_ADC_NUM_PINS] =
    PIOS_ADC_CHANNEL_MAPPING;

/**
 * @brief Initialise the ADC Peripheral
 * @param[in] adc_oversample
 * @return 
 *  @arg 1 for success
 *  @arg 0 for failure
 * Currently ignores rates and uses hardcoded values.  Need a little logic to
 * map from sampling rates and such to ADC constants.
 */
uint8_t AHRS_ADC_Config(int32_t adc_oversample)
{

	int32_t i;

	adc_config.adc_oversample = adc_oversample;
	
	ADC_DeInit(ADC1);
	ADC_DeInit(ADC2);

	/* Setup analog pins */
	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_StructInit(&GPIO_InitStructure);
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;

	/* Enable each ADC pin in the array */
	for (i = 0; i < PIOS_ADC_NUM_PINS; i++) {
		GPIO_InitStructure.GPIO_Pin = ADC_GPIO_PIN[i];
		GPIO_Init(ADC_GPIO_PORT[i], &GPIO_InitStructure);
	}

	/* Enable ADC clocks */
	PIOS_ADC_CLOCK_FUNCTION;

	/* Map channels to conversion slots depending on the channel selection mask */
	for (i = 0; i < PIOS_ADC_NUM_PINS; i++) {
		ADC_RegularChannelConfig(ADC_MAPPING[i], ADC_CHANNEL[i],
					 ADC_CHANNEL_MAPPING[i],
					 PIOS_ADC_SAMPLE_TIME);
	}

#if (PIOS_ADC_USE_TEMP_SENSOR)
	ADC_TempSensorVrefintCmd(ENABLE);
	ADC_RegularChannelConfig(PIOS_ADC_TEMP_SENSOR_ADC, ADC_Channel_14,
				 PIOS_ADC_TEMP_SENSOR_ADC_CHANNEL,
				 PIOS_ADC_SAMPLE_TIME);
#endif

	// TODO: update ADC to continuous sampling, configure the sampling rate
	/* Configure ADCs */
	ADC_InitTypeDef ADC_InitStructure;
	ADC_StructInit(&ADC_InitStructure);
	ADC_InitStructure.ADC_Mode = ADC_Mode_RegSimult;
	ADC_InitStructure.ADC_ScanConvMode = ENABLE;
	ADC_InitStructure.ADC_ContinuousConvMode = ENABLE;
	ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
	ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
	ADC_InitStructure.ADC_NbrOfChannel =
	    ((PIOS_ADC_NUM_CHANNELS + 1) >> 1);
	ADC_Init(ADC1, &ADC_InitStructure);

#if (PIOS_ADC_USE_ADC2)
	ADC_Init(ADC2, &ADC_InitStructure);

	/* Enable ADC2 external trigger conversion (to synch with ADC1) */
	ADC_ExternalTrigConvCmd(ADC2, ENABLE);
#endif

	RCC_ADCCLKConfig(PIOS_ADC_ADCCLK);
	RCC_PCLK2Config(RCC_HCLK_Div16);

	/* Enable ADC1->DMA request */
	ADC_DMACmd(ADC1, ENABLE);

	/* ADC1 calibration */
	ADC_Cmd(ADC1, ENABLE);
	ADC_ResetCalibration(ADC1);
	while (ADC_GetResetCalibrationStatus(ADC1)) ;
	ADC_StartCalibration(ADC1);
	while (ADC_GetCalibrationStatus(ADC1)) ;

#if (PIOS_ADC_USE_ADC2)
	/* ADC2 calibration */
	ADC_Cmd(ADC2, ENABLE);
	ADC_ResetCalibration(ADC2);
	while (ADC_GetResetCalibrationStatus(ADC2)) ;
	ADC_StartCalibration(ADC2);
	while (ADC_GetCalibrationStatus(ADC2)) ;
#endif

	/* Enable DMA1 clock */
	RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

	/* Configure DMA1 channel 1 to fetch data from ADC result register */
	DMA_InitTypeDef DMA_InitStructure;
	DMA_StructInit(&DMA_InitStructure);
	DMA_DeInit(DMA1_Channel1);
	DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t) & ADC1->DR;
	DMA_InitStructure.DMA_MemoryBaseAddr =
	    (uint32_t) & raw_data_buffer[0];
	DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
	/* We are double buffering half words from the ADC.  Make buffer appropriately sized */
	DMA_InitStructure.DMA_BufferSize =
	    (PIOS_ADC_NUM_CHANNELS * adc_oversample * 2) >> 1;
	DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
	DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
	/* Note: We read ADC1 and ADC2 in parallel making a word read, also hence the half buffer size */
	DMA_InitStructure.DMA_PeripheralDataSize =
	    DMA_PeripheralDataSize_Word;
	DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Word;
	DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
	DMA_InitStructure.DMA_Priority = DMA_Priority_High;
	DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
	DMA_Init(DMA1_Channel1, &DMA_InitStructure);
	DMA_Cmd(DMA1_Channel1, ENABLE);

	/* Trigger interrupt when for half conversions too to indicate double buffer */
	DMA_ITConfig(DMA1_Channel1, DMA_IT_TC, ENABLE);
	DMA_ITConfig(DMA1_Channel1, DMA_IT_HT, ENABLE);

	/* Configure and enable DMA interrupt */
	NVIC_InitTypeDef NVIC_InitStructure;
	NVIC_InitStructure.NVIC_IRQChannel = DMA1_Channel1_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority =
	    PIOS_ADC_IRQ_PRIO;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

	/* Finally start initial conversion */
	ADC_SoftwareStartConvCmd(ADC1, ENABLE);

	/* Use simple averaging filter for now */
	for (int i = 0; i < adc_oversample; i++)
		adc_config.fir_coeffs[i] = 1;
	adc_config.fir_coeffs[adc_oversample] = adc_oversample;
	
	return 1;
}

/**
 * @brief Set a callback function that is executed whenever
 * the ADC double buffer swaps 
 */
void AHRS_ADC_SetCallback(ADCCallback new_function) 
{
	callback_function = new_function;
}

/**
 * @brief Return the address of the downsampled data buffer 
 */
float * AHRS_ADC_GetBuffer(void)
{
	return downsampled_buffer;
}

/**
 * @brief Return the address of the raw data data buffer 
 */
int16_t * AHRS_ADC_GetRawBuffer(void)
{
	return (int16_t *) adc_config.valid_data_buffer;
}

/**
 * @brief Return the amount of over sampling
 */
uint8_t AHRS_ADC_GetOverSampling(void)
{
    return adc_config.adc_oversample;
}

/**
 * @brief Set the fir coefficients.  Takes as many samples as the 
 * current filter order plus one (normalization)
 *
 * @param new_filter Array of adc_oversampling floats plus one for the
 * filter coefficients
 */
void AHRS_ADC_SetFIRCoefficients(float * new_filter)
{
	// Less than or equal to get normalization constant
	for(int i = 0; i <= adc_config.adc_oversample; i++) 
		adc_config.fir_coeffs[i] = new_filter[i];
}

/**
 * @brief Downsample the data for each of the channels then call
 * callback function if installed
 */ 
void AHRS_ADC_downsample_data()
{
	uint16_t chan;
	uint16_t sample;
	
	for (chan = 0; chan < PIOS_ADC_NUM_CHANNELS; chan++)
	{
		register int32_t sum = 0;
		for (sample = 0; sample < adc_config.adc_oversample; sample++)
			sum += (int32_t)adc_config.valid_data_buffer[chan + sample * PIOS_ADC_NUM_CHANNELS] * adc_config.fir_coeffs[sample];
		downsampled_buffer[chan] = (float)sum / adc_config.fir_coeffs[adc_config.adc_oversample];
	}
	
	if (callback_function)
		callback_function(downsampled_buffer);
}

/**
 * @brief Interrupt for half and full buffer transfer
 * 
 * This interrupt handler swaps between the two halfs of the double buffer to make
 * sure the ahrs uses the most recent data.  Only swaps data when AHRS is idle, but
 * really this is a pretense of a sanity check since the DMA engine is consantly 
 * running in the background.  Keep an eye on the ekf_too_slow variable to make sure
 * it's keeping up.
 */
void AHRS_ADC_DMA_Handler(void)
{
	if (DMA_GetFlagStatus(DMA1_IT_TC1)) {	// whole double buffer filled 
		adc_config.valid_data_buffer =
			&raw_data_buffer[1 * PIOS_ADC_NUM_CHANNELS *
				 adc_config.adc_oversample];
		DMA_ClearFlag(DMA1_IT_TC1);
		AHRS_ADC_downsample_data();
	}
	else if (DMA_GetFlagStatus(DMA1_IT_HT1)) {
		adc_config.valid_data_buffer = 
			&raw_data_buffer[0 * PIOS_ADC_NUM_CHANNELS *
				 adc_config.adc_oversample];
		DMA_ClearFlag(DMA1_IT_HT1);
		AHRS_ADC_downsample_data();
	}
	else {
		// This should not happen, probably due to transfer errors
		DMA_ClearFlag(DMA1_FLAG_GL1);
	}
}
