/*
 * convolution.c
 *
 *  Created on: Sep 20, 2015
 *      Author: Dawson
 */

#define NANOSECONDS_IN_A_SECOND			1000000000

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <sndfile.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <portaudio.h>
#include <ncurses.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <pthread.h>
#include "dawsonaudio.h"
#include "convolve.h"
#include "vector.h"
#include "impulse.h"

typedef struct FFTArgs {
	int first_sample_index;
	int last_sample_index;
	int impulse_block_number;
	int num_callbacks_to_complete;
	int counter;
} FFTArgs;

FFTData *g_fftData_ptr; // Stores the Fourier-transforms of each block of the impulse
InputAudioData *g_inputAudioData_ptr; // Stores data for input audio

int g_block_length; // The length in frames of each audio buffer received by portaudio
int g_impulse_length; // The length in frames of the impulse
int g_num_blocks; // The number of equal-size blocks into which the impulse will be divided
int g_max_factor; // The highest power of 2 used to divide the impulse into blocks
int g_input_storage_buffer_length; // The length of the buffer used to store incoming audio from the mic
int g_output_storage_buffer_length;
int g_end_sample; // The index of the last sample in g_storage_buffer
int g_counter = 0; // Keep track of how many callback cycles have passed

long g_block_duration_in_nanoseconds = (NANOSECONDS_IN_A_SECOND / SAMPLE_RATE)
		* MIN_FFT_BLOCK_SIZE;

Vector g_powerOf2Vector; // Stores the correct number of powers of 2 to make the block calculations work (lol)

pthread_t thread;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

/*
 * This buffer is used to store INCOMING audio from the mic.
 */
float *g_input_storage_buffer;

/*
 * This buffer is used to store OUTGOING audio that has been processed.
 */
float *g_output_storage_buffer;

void initializeGlobalParameters() {
	g_block_length = MIN_FFT_BLOCK_SIZE;
	g_num_blocks = g_impulse_length / g_block_length;
	g_max_factor = g_num_blocks / 4;
	g_input_storage_buffer_length = g_impulse_length / 4;
	g_output_storage_buffer_length = g_input_storage_buffer_length * 2;
	g_end_sample = g_input_storage_buffer_length - 1;

	// Allocate memory for storage buffer, fill with 0s.
	g_input_storage_buffer = (float *) calloc(g_input_storage_buffer_length,
			sizeof(float));

	// Allocate memory for output storage buffer, fill with 0s.
	g_output_storage_buffer = (float *) calloc(g_output_storage_buffer_length,
			sizeof(float));

}

/*
 * This function takes an FFT of a portion of audio from the g_input_storage_buffer,
 * zero-pads it to twice its length, multiplies it with a specific block of FFT
 * data from the impulse, takes the IFFT of the resulting data, and places that
 * data in the g_output_storage_buffer after the correct amount of callback cycles.
 */
void *calculateFFT(void *incomingFFTArgs) {

	FFTArgs *fftArgs = (FFTArgs *) incomingFFTArgs;

	int i;

	int multiplier = 1;

	if (fftArgs->impulse_block_number % 2 != 0) {
		multiplier = 1;
	} else {
		multiplier = 2;
	}

	int numCyclesToWait = fftArgs->num_callbacks_to_complete * multiplier - 1;

	int counter_target = (fftArgs->counter + numCyclesToWait)
			% (g_max_factor * 2);
	if (counter_target == 0) {
		counter_target = g_max_factor * 2;
	}

//	printf("waiting for %d cycles to complete.\n", fftArgs->num_callbacks_to_complete * multiplier);

//	printf("counter_target: %d\n", counter_target);

	pthread_detach(pthread_self());

//	 TODO: Implement fft multiplication and ifft here
//	 Steps:
//	 1. Create buffer with length = 2 * (last_sample_index - first_sample_index),
//	    fill the buffer with 0s.
	int blockLength = fftArgs->last_sample_index - fftArgs->first_sample_index
			+ 1;
	int convLength = blockLength * 2;
//	printf("convLength: %d\n", convLength);
	complex *inputAudio = calloc(convLength, sizeof(complex));
	// 2. Take audio from g_input_storage_buffer (first_sample_index to last_sample_index)
	//    and place it into the buffer created in part 1 (0 to (last_sample_index - first_sample_index)).
	for (i = 0; i < blockLength; i++) {
		inputAudio[i].Re = g_input_storage_buffer[fftArgs->first_sample_index
				+ i];
	}

	printf(
			"Thread %d: Start convolving sample %d to %d with h%d. This process will take %d cycles and complete when N = %d.\n",
			pthread_self(), fftArgs->first_sample_index, fftArgs->last_sample_index,
			fftArgs->impulse_block_number, numCyclesToWait, counter_target);

//	printf("Thread has acquired its samples\n");
	// 3. Take the FFT of the buffer created in part 1.
	complex *temp = calloc(convLength, sizeof(complex));
	fft(inputAudio, convLength, temp);
	// 4. Determine correct impulse FFT block based in impulse_block_number. The length of this
	//	  block should automatically be the same length as the length of the buffer created in part 1
	//    that now holds the input audio data.
	int fftBlockNumber = fftArgs->impulse_block_number;
	// 5. Create buffer of length 2 * (last_sample_index - first_sample_index) to hold the result of
	//    FFT multiplication.
	complex *convResult = calloc(convLength, sizeof(complex));
	// 6. Complex multiply the buffer created in part 1 with the impulse FFT block determined in part 4,
	//    and store the result in the buffer created in part 5.
	complex c;
	for (i = 0; i < convLength; i++) {
//		printf("inputAudio[%d].Re = %f\n", i, inputAudio[i].Re);
//		printf("fftBlocks[%d][%d].Re = %f\n", fftBlockNumber, i, g_fftData_ptr->fftBlocks[fftBlockNumber][i]);
		c = complex_mult(inputAudio[i],
				g_fftData_ptr->fftBlocks[fftBlockNumber][i]);

		convResult[i].Re = c.Re;
		convResult[i].Im = c.Im;
//		printf("convResult[%d].Re = %f\n", i, convResult[i].Re);

	}
	// 7. Take the IFFT of the buffer created in part 5.
	ifft(convResult, convLength, temp);

	/* Take just the first N elements and find the largest value for scaling purposes */
//	float maxY = 0;
//	for (i = 0; i < convLength; i++) {
//		if (fabsf(convResult[i].Re) > maxY) {
//			maxY = fabsf(convResult[i].Re);
//		}
//	}
//	printf("maxY = %f\n", maxY);
	// 8. When the appropriate number of callback cycles have passed (num_callbacks_to_complete), put
	//    the real values of the buffer created in part 5 into the g_output_storage_buffer
	//    (sample 0 through sample 2 * (last_sample_index - first_sample_index)
	while (g_counter != counter_target) {
		nanosleep((const struct timespec[] ) { {0,g_block_duration_in_nanoseconds/2}}, NULL);
	}

//		printf("\n\n\nDATA:\n\n\n");

	// Put data in output buffer
	for (i = 0; i < convLength; i++) {
//			printf("convResult[%d].Re: %f\n", i, convResult[i].Re);
		g_output_storage_buffer[i] += convResult[i].Re;
		//			printf("g_output_storage_buffer[%d]: %f\n", i, g_output_storage_buffer[i]);
	}

	printf(
			"Thread %d: The result of the convolution of sample %d to %d with h%d has been added to the output buffer. Expected arrival: when n = %d.\n",
			pthread_self(), fftArgs->first_sample_index, fftArgs->last_sample_index,
			fftArgs->impulse_block_number, counter_target);

	// Free all buffers
	free(temp);
	free(convResult);
	free(inputAudio);

	/*
	 * How to determine when an appropriate number of callback cycles have passed? When this function is
	 * called, store the current value of g_counter in a local int variable.
	 *
	 * Based on the fact that we know what the counter was at the start, and how many callback cycles are
	 * needed, we can predict what the counter will be when the audio is needed, and we will store this value
	 * in another local int variable.
	 *
	 * When the g_counter reaches the value of this second int variable, then we know we are ready to deliver
	 * the audio to the g_output_storage_buffer.
	 */

//	printf("\n\nThread returning\n\n");
	free(fftArgs);
	pthread_exit(NULL);
	return NULL;
}

/*
 *  Description:  Callback for Port Audio
 */
static int paCallback(const void *inputBuffer, void *outputBuffer,
		unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
		PaStreamCallbackFlags statusFlags, void *userData) {

	float *inBuf = (float*) inputBuffer;
	float *outBuf = (float*) outputBuffer;

	int i, j;

	for (i = 0; i < framesPerBuffer; i++) {
//		printf("outBuf[%d]: %f\n", i, outBuf[i]);
		outBuf[i] = g_output_storage_buffer[i];
	}

	++g_counter;

	if (g_counter >= g_max_factor * 2 + 1) {
		g_counter = 1;
	}

	printf("N = %d\n", g_counter);

	// 1. Shift both input and output audio buffers

	// Shift g_input_storage_buffer to the left by g_block_length
	for (i = 0; i < g_input_storage_buffer_length - g_block_length; i++) {
		g_input_storage_buffer[i] = g_input_storage_buffer[i + g_block_length];
	}

	// 2. Copy input audio to g_input_storage_buffer
	// Fill right-most portion of g_input_storage_buffer with most recent audio
	for (i = 0; i < g_block_length; i++) {
		g_input_storage_buffer[g_input_storage_buffer_length - g_block_length
				+ i] = inBuf[i] * 0.00001f;
	}

	// 3. Create all new threads
	/*
	 * This will be replaced eventually by code which actually performs the convolution
	 */
	for (j = 0; j < 1; j++) {
//	for (j = 0; j < g_powerOf2Vector.size; j++) {
		int factor = vector_get(&g_powerOf2Vector, j);
//		printf("factor: %d\n", factor);
		if (g_counter % factor == 0 && g_counter != 0) {

			/*
			 * Take the specified samples from the input_storage_buffer, zero-pad them to twice their
			 * length, FFT them, multiply the resulting spectrum by the corresponding impulse FFT block,
			 * IFFT the result, put the result in the output_storage_buffer.
			 */
//			printf(
//					"Start convolving block %d to %d (sample %d to %d) with h%d\n",
//					(g_counter - factor), g_counter,
//					(1 + g_end_sample - g_block_length * factor), g_end_sample,
//					(j * 2 + 1));
			FFTArgs *fftArgs = (FFTArgs *) malloc(sizeof(FFTArgs));

			fftArgs->first_sample_index = (1 + g_end_sample
					- g_block_length * factor);
			fftArgs->last_sample_index = g_end_sample;
			fftArgs->impulse_block_number = (j * 2 + 1);
			fftArgs->num_callbacks_to_complete = factor;
			fftArgs->counter = g_counter;
//			if (j == 6) {
				pthread_create(&thread, NULL, calculateFFT, (void *) fftArgs);
//			}

//			printf(
//					"Start convolving block %d to %d (sample %d to %d) with h%d\n",
//					(g_counter - factor), g_counter,
//					(1 + g_end_sample - g_block_length * factor), g_end_sample,
//					(j * 2 + 2));
			FFTArgs *fftArgs2 = (FFTArgs *) malloc(sizeof(FFTArgs));
			fftArgs2->first_sample_index = (1 + g_end_sample
					- g_block_length * factor);
			fftArgs2->last_sample_index = g_end_sample;
			fftArgs2->impulse_block_number = (j * 2 + 2);
			fftArgs2->num_callbacks_to_complete = factor;
			fftArgs2->counter = g_counter;

//			if (j == 5) {
				pthread_create(&thread, NULL, calculateFFT, (void *) fftArgs2);
//			}

		}
	}

	// 4. All new threads MUST acquire correct audio blocks from g_input_audio_buffer
	// and this must happen before a new portaudio callback cycle begins

	// 5. All finishing threads MUST deliver their audio to g_output_audio_buffer and
	// this must also happen before a new portaudio callback cycle begins

	// 6. Audio copied from g_output_audio_buffer to outBuf

	// FFT of block
//	{
//		complex *signal = calloc(framesPerBuffer, sizeof(complex));
//		complex *temp = calloc(framesPerBuffer, sizeof(complex));
//		for (i = 0; i < framesPerBuffer; i++) {
//			signal[i].Re = inBuf[i];
//		}
//
//		fft(signal, framesPerBuffer, temp);
//
//		ifft(signal, framesPerBuffer, temp);
//
//		for (i = 0; i < framesPerBuffer; i++) {
//			outBuf[i] = signal[i].Re / 50;
//		}
//
//		free(signal);
//		free(temp);
//	}

	// Shift g_output_storage_buffer
	for (i = 0; i < g_output_storage_buffer_length - g_block_length; i++) {
		g_output_storage_buffer[i] =
				g_output_storage_buffer[i + g_block_length];
	}

	// Get audio from g_input_storage_buffer
//	for (i = 0; i < g_block_length; i++) {
//		outBuf[i] = g_input_storage_buffer[g_input_storage_buffer_length
//				- g_block_length + i];
//	}

	return paContinue;
}

void runPortAudio() {
	PaStream* stream;
	PaStreamParameters outputParameters;
	PaStreamParameters inputParameters;
	PaError err;
	/* Initialize PortAudio */
	Pa_Initialize();
	/* Set output stream parameters */
	outputParameters.device = Pa_GetDefaultOutputDevice();
	outputParameters.channelCount = MONO;
	outputParameters.sampleFormat = paFloat32;
	outputParameters.suggestedLatency = Pa_GetDeviceInfo(
			outputParameters.device)->defaultLowOutputLatency;
	outputParameters.hostApiSpecificStreamInfo = NULL;
	/* Set input stream parameters */
	inputParameters.device = Pa_GetDefaultInputDevice();
	inputParameters.channelCount = MONO;
	inputParameters.sampleFormat = paFloat32;
	inputParameters.suggestedLatency =
			Pa_GetDeviceInfo(inputParameters.device)->defaultLowInputLatency;
	inputParameters.hostApiSpecificStreamInfo = NULL;
	/* Open audio stream */
	err = Pa_OpenStream(&stream, &inputParameters, &outputParameters,
	SAMPLE_RATE, MIN_FFT_BLOCK_SIZE, paNoFlag, paCallback, NULL);

	printf("\nPa buffer size: %d\n", MIN_FFT_BLOCK_SIZE);
	printf("g_input_storage_buffer size: %d\n", g_input_storage_buffer_length);
	printf("g_output_storage_buffer size: %d\n",
			g_output_storage_buffer_length);
	printf("g_num_blocks: %d\n", g_num_blocks);
	printf("g_block_length: %d\n", g_block_length);
	printf("g_max_factor: %d\n", g_max_factor);
	printf("g_impulse_length: %d\n\n", g_impulse_length);

	if (err != paNoError) {
		printf("PortAudio error: open stream: %s\n", Pa_GetErrorText(err));
	}
	/* Start audio stream */
	err = Pa_StartStream(stream);
	if (err != paNoError) {
		printf("PortAudio error: start stream: %s\n", Pa_GetErrorText(err));
	}
	/* Get user input */
	char ch = '0';
	while (ch != 'q') {
		printf("Press 'q' to finish execution: ");
		ch = getchar();
	}
	err = Pa_StopStream(stream);
	/* Stop audio stream */
	if (err != paNoError) {
		printf("PortAudio error: stop stream: %s\n", Pa_GetErrorText(err));
	}
	/* Close audio stream */
	err = Pa_CloseStream(stream);
	if (err != paNoError) {
		printf("PortAudio error: close stream: %s\n", Pa_GetErrorText(err));
	}
	/* Terminate audio stream */
	err = Pa_Terminate();
	if (err != paNoError) {
		printf("PortAudio error: terminate: %s\n", Pa_GetErrorText(err));
	}
}

void loadImpulse() {
	audioData* impulse = fileToBuffer("churchIR.wav");
	printf("impulse length before zero-padding: %d\n", impulse->numFrames);
	impulse = zeroPadToNextPowerOfTwo(impulse);
	g_impulse_length = impulse->numFrames;
//	int i;
//	for (i=0; i<impulse->numFrames; i++) {
//		printf("impulse->buffer[%d]: %f\n", i, impulse->buffer[i]);
//	}
	Vector blockLengthVector = determineBlockLengths(impulse);
	BlockData* data_ptr = allocateBlockBuffers(blockLengthVector);
	partitionImpulseIntoBlocks(blockLengthVector, data_ptr, impulse);
	g_fftData_ptr = allocateFFTBuffers(data_ptr, blockLengthVector);
	g_inputAudioData_ptr = allocateInputAudioBuffers(blockLengthVector);
//	printPartitionedImpulseData(blockLengthVector, data_ptr);
//	printPartitionedImpulseFFTData(blockLengthVector,data_ptr);

	// This part below tests that the impulse is broken up correctly

//	int i,j;
//
//	for (i=0; i<data_ptr->size; i++) {
//
////
//		float *data = (float *) malloc(sizeof(float) * blockLengthVector.data[i]);
//
//
//		char fileName[15];
//
//		sprintf(fileName, "file%d.wav", i);
//
//		writeWavFile(data_ptr->audioBlocks[i],44100,1,blockLengthVector.data[i],1,fileName);
//	}
}

void initializePowerOf2Vector() {
	vector_init(&g_powerOf2Vector);
	int counter = 0;
	while (pow(2, counter) <= g_max_factor) {
		vector_append(&g_powerOf2Vector, pow(2, counter++));
	}
}

int main(int argc, char **argv) {

	printf("Block duration in nanoseconds: %lu\n",
			g_block_duration_in_nanoseconds);

	loadImpulse();

	initializeGlobalParameters();

	initializePowerOf2Vector();

	runPortAudio();

	return 0;
}

