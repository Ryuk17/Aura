/*
 * lawliet_fft.cpp
 *
 *  Created on: 2022年3月15日
 *      Author: ryuk
 */

#include <iostream>
#include <math.h>

#include "NE10_dsp.h"

#include "lawliet_macros.h"
#include "lawliet_fft.h"

void lawliet_fft_create(FFTHandle *fft_handle, FFTInit *fft_init)
{
	if(fft_handle == NULL || fft_init == NULL)
	{
		LAWLIET_LOG(LAWLIET_ERROR, "%s", "The fft_handle or fft_init is NULL");
		return ;
	}

	fft_handle->nfft = fft_init->nfft;
	fft_handle->cfg = ne10_fft_alloc_r2c_float32(fft_handle->nfft);
	fft_handle->window.resize(fft_handle->nfft);

	for(int i=0; i<fft_handle->nfft; i++)
	{
		switch(fft_init->window_type)
		{
			case LAWLIET_HAMMING_WINDOW:
				fft_handle->window[i] = 0.54 - 0.46 * cos(2*PI*i/fft_handle->nfft);
				break;

			case LAWLIET_HANNING_WINDOW:
				fft_handle->window[i] = 0.5 - 0.5 * cos(2*PI*i/fft_handle->nfft);
				break;

			case LAWLIET_RECTANGLE_WINDOW:
				fft_handle->window[i] = 1;
				break;

			default:
				LAWLIET_LOG(LAWLIET_ERROR, "%s", "The fft_handle or fft_init is NULL");
				return;
		}
	}
	fft_handle->checked = 1;
}


void lawliet_fft(FFTHandle *fft_handle, float *input, float *output)
{
	if(fft_handle == NULL)
	{
		LAWLIET_LOG(LAWLIET_ERROR, "%s", "The fft_handle is NULL");
	}

	if(fft_handle->checked == 0)
	{
		LAWLIET_LOG(LAWLIET_ERROR, "%s", "The fft_handle has not initialized");
	}

	for(int i=0; i<fft_handle->nfft; i++)
	{
		input[i] = input[i] * fft_handle->window[i];
	}

#if __ARM_NEON__
	ne10_fft_r2c_1d_float32_neon(output, input, fft_handle->cfg);
#else
	ne10_fft_r2c_1d_float32_c((ne10_fft_cpx_float32_t*)output, input, fft_handle->cfg);
#endif
}

void lawliet_ifft(FFTHandle *fft_handle, float *input, float *output)
{
	if(fft_handle == NULL)
	{
		LAWLIET_LOG(LAWLIET_ERROR, "%s", "The fft_handle is NULL");
	}

	if(fft_handle->checked == 0)
	{
		LAWLIET_LOG(LAWLIET_ERROR, "%s", "The fft_handle has not initialized");
	}

#if __ARM_NEON__
	  ne10_fft_c2r_1d_float32_neon(output, input, fft_handle->cfg);
#else
	  ne10_fft_c2r_1d_float32_c(output, (ne10_fft_cpx_float32_t*)input, fft_handle->cfg);
#endif
}


void lawliet_fft_destory(FFTHandle *fft_handle)
{
	if(fft_handle == NULL)
	{
		LAWLIET_LOG(LAWLIET_ERROR, "%s", "The fft_handle is NULL");
	}

	ne10_fft_destroy_r2c_float32(fft_handle->cfg);
	fft_handle->checked = 0;

}

