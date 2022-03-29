//============================================================================
// Name        : lawliet_fft_test.cpp
// Author      : 
// Version     :
// Copyright   : Your copyright notice
// Description : Hello World in C++, Ansi-style
//============================================================================

#include <iostream>
using namespace std;

#include "lawliet_fft.h"

#define N_FFT 512

int main() {
    char inFileName[512];
    char outFileName[512];

    short input[N_FFT];

    FILE *in;
    FILE *out;

    sprintf(inFileName, "%s", "./sample/fft-test.wav");
    sprintf(outFileName, "%s", "./sample/fft-out.wav");

    printf("Open %s\n", inFileName);
    in = fopen(inFileName, "rb");
    if(in == NULL)
    {
        perror("Error: ");
        return -1;
    }

    printf("Open %s\n", outFileName);
    out = fopen(outFileName, "wb");
    if(out == NULL)
    {
        perror("Error: ");
        return -1;
    }

    fread(input, 44, sizeof(short), in);
    fwrite(input, 44, sizeof(short), out);


    FFTInit fft_init;
    FFTHandle fft_handle;
    fft_init.nfft = N_FFT;
    fft_init.window_type = LAWLIET_RECTANGLE_WINDOW;

    lawliet_fft_create(&fft_handle, &fft_init);

    float *input_buf = (float *)calloc(N_FFT, sizeof(float));
    float *output_buf = (float *)calloc(N_FFT + 2, sizeof(float));

    while(fread(input, N_FFT, sizeof(short), in))
    {
    	for(int i=0; i<N_FFT;i++)
    	{
    		input_buf[i] = (float)input[i];
    	}

    	lawliet_fft(&fft_handle, input_buf, output_buf);
    	lawliet_ifft(&fft_handle, output_buf, input_buf);

    	for(int i=0; i<N_FFT;i++)
    	{
    		input[i] = (short)input_buf[i];
    	}

    	fwrite(input, N_FFT, sizeof(short), out);
    }

    lawliet_fft_destory(&fft_handle);

    free(input_buf);
    free(output_buf);

    fclose(in);
    fclose(out);

	cout << "LAWLIET FFT TEST FINISHED" << endl;
	return 0;
}
