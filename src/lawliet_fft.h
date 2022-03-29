/*
 * lawliet_fft.h
 *
 *  Created on: 2022年3月15日
 *      Author: ryuk
 */

#ifndef LAWLIET_FFT_H_
#define LAWLIET_FFT_H_

#include <vector>

#include "NE10_dsp.h"

typedef enum
{
	LAWLIET_HAMMING_WINDOW,
	LAWLIET_HANNING_WINDOW,
	LAWLIET_RECTANGLE_WINDOW,
}FFTWindow;

typedef struct
{
	int nfft;
	FFTWindow window_type;
}FFTInit;

typedef struct{
	int checked;
	int nfft;
	std::vector<float> window;
	ne10_fft_r2c_cfg_float32_t cfg;
}FFTHandle;

void lawliet_fft_create(FFTHandle *fft_handle, FFTInit *fft_init);
void lawliet_fft(FFTHandle *fft_handle, float *input, float *output);
void lawliet_ifft(FFTHandle *fft_handle, float *input, float *output);
void lawliet_fft_destory(FFTHandle *fft_handle);


#endif /* LAWLIET_FFT_H_ */
