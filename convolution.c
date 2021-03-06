/*
 * convolution.c
 *
 *  Created on: Sep 20, 2015
 *      Author: Dawson
 */

#define NANOSECONDS_IN_A_SECOND			1000000000
#define NUM_CHECKS_PER_CYCLE			2
#define FFT_SIZE 						MIN_FFT_BLOCK_SIZE
#define SMOOTHING_AMT					2048
#define HALF_FFT_SIZE 					FFT_SIZE/2
#define SAMPLES_PER_MS					SAMPLE_RATE/1000
#define LEFT							1
#define RIGHT							2
#define LIVE_AUDIO_INPUT				true
#define AUDIO_FILE_INPUT				!LIVE_AUDIO_INPUT
#define IMPULSE_FILE_NAME				"resources/impulses/Factory Hall.wav"
#define AUDIO_FILE_NAME					"resources/audio/sax.wav"

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
#include "fft.h"
#include <GLUT/glut.h>

GLsizei g_width = 1200;
GLsizei g_height = 700;
GLsizei g_last_width = 1200;
GLsizei g_last_height = 700;
GLfloat g_mouse_x;
GLfloat g_mouse_y;
GLfloat g_relative_width = 8.0f;
GLfloat g_height_top = 3;
GLfloat g_height_bottom = -3;

GLsizei g_current_index = -1;

GLenum g_fillmode = GL_FILL;

GLboolean g_fullscreen = false;
GLboolean g_drawempty = false;
GLboolean g_ready = false;
GLboolean a_pressed = false;
GLboolean z_pressed = false;

GLfloat g_angle_y = 0.0f;
GLfloat g_angle_x = 0.0f;
GLfloat g_inc = 0.0f;
GLfloat g_inc_y = 0.0f;
GLfloat g_inc_x = 0.0f;
GLfloat g_linewidth = 1.0f;

float top_vals_left[HALF_FFT_SIZE];
float bottom_vals_left[HALF_FFT_SIZE];
float top_vals_right[HALF_FFT_SIZE];
float bottom_vals_right[HALF_FFT_SIZE];
float *g_amp_envelope;
float g_max = 0.0f;

char *audio_file_name = NULL;

float last_input_spectrum[15][HALF_FFT_SIZE];

complex *input_spectrum;

int g_dry_wet = 50;

bool g_drawing = false;

float g_loudest = 0.0f;
int g_consecutive_skipped_cycles = 0;

float x_values[HALF_FFT_SIZE];

typedef struct GraphData
{
	int *x_indices;
	float *y_values;
	int first_index;
	int last_index;
	int length;
} GraphData;
// For displaying impulse

typedef struct ImpulseWindow {
	int edge_margin;
	int margin_top;
	int margin_bottom;
	int margin_left;
	int margin_right;
	int top_line;
	int bottom_line;
	int mid_line;
	float top_line_percent;
	float mid_line_percent;
	float bottom_line_percent;
	bool top_line_selected;
	bool mid_line_selected;
	bool bottom_line_selected;
} ImpulseWindow;

ImpulseWindow *impulseWindow = NULL;

typedef struct Point {
	int x;
	int y;
} Point;

typedef struct ImpulseHorizontalSelect {
	Point top_left;
	Point top_right;
	Point bottom_left;
	Point bottom_right;
	int left_bound;
	int right_bound;
	float left_bound_percent;
	float right_bound_percent;
	bool left_bound_selected;
	bool right_bound_selected;
	bool dragging;
	int drag_begin;
	int drag_end;
} ImpulseHorizontalSelect;

ImpulseHorizontalSelect *impulseHorizontalSelect = NULL;

// Length of crossover between synthesized and recorded impulse
int crossover_length = 200 * SAMPLES_PER_MS;

// Location of point at which crossover between synthesized and recorded impulse begins
int crossover_point = 50 * SAMPLES_PER_MS;

int g_impulse_num_frames;

int g_changes_made = 0;

int g_current_channel_view = LEFT;

int g_r_pressed = false;

float synthesized_impulse_gain_factor = 0.4f;

float g_interpolation_amt;

int g_input_sensitivity;

bool use_attack_from_impulse = true;

bool smooth_draw = true;

int smooth_draw_amt = 10;

bool g_changingImpulse = false;

unsigned int g_channels = MONO;

typedef void (*ButtonCallback)();
typedef void (*SliderCallback)();

typedef struct Slider {
	int x_min;
	int x_max;
	int x_pos;
	int y;
	int min_val;
	int max_val;
	int current_val;
	char *label;
	int state;
	SliderCallback callbackFunction;
} Slider;

typedef struct Button {
	int x;
	int y;
	int w;
	int h;
	int state;
	int highlighted;
	char *label;
	ButtonCallback callbackFunction;
} Button;

typedef struct Mouse
{
	int x;		/*	the x coordinate of the mouse cursor	*/
	int y;		/*	the y coordinate of the mouse cursor	*/
	int lmb;	/*	is the left button pressed?		*/
	int mmb;	/*	is the middle button pressed?	*/
	int rmb;	/*	is the right button pressed?	*/

	/*
	 *	These two variables are a bit odd. Basically I have added these to help replicate
	 *	the way that most user interface systems work. When a button press occurs, if no
	 *	other button is held down then the co-ordinates of where that click occured are stored.
	 *	If other buttons are pressed when another button is pressed it will not update these
	 *	values.
	 *
	 *	This allows us to "Set the Focus" to a specific portion of the screen. For example,
	 *	in maya, clicking the Alt+LMB in a view allows you to move the mouse about and alter
	 *	just that view. Essentually that viewport takes control of the mouse, therefore it is
	 *	useful to know where the first click occured....
	 */
	int xpress; /*	stores the x-coord of when the first button press occurred	*/
	int ypress; /*	stores the y-coord of when the first button press occurred	*/
} Mouse;

Mouse TheMouse = {0,0,0,0,0};

typedef struct {
	float amplitude1;
	float sampleRate;
	SNDFILE *infile1;
	SF_INFO sfinfo1;
	int channels;
	float buffer1[MIN_FFT_BLOCK_SIZE*2];
} paData;

void RecomputeImpulseButtonCallback();
void RandomizeButtonCallback();
void ReverseButtonCallback();
void SmoothnessSliderCallback();
void InterpolationSliderCallback();
void ImpulseLengthSliderCallback();
void ChannelSliderCallback();
void DryWetSliderCallback();
void InputSensitivitySliderCallback();

Button RecomputeImpulseButton = {5,5,100,25,0,0," Recompute Impulse ", RecomputeImpulseButtonCallback };

Button RandomizeButton = {115,5,100,25,0,0," Randomize ", RandomizeButtonCallback };

Button ReverseButton = {225, 5, 100, 25, 0, 0, "Reverse Bins", ReverseButtonCallback };

Slider DryWetSlider = {465, 555, 510, 65, 0, 100, 50, "Dry/Wet", 0, DryWetSliderCallback };

Slider SmoothnessSlider = {15,105,60,65,1,22,12,"Smoothness",0, SmoothnessSliderCallback };

Slider InterpolationSlider = {125,215,125,65,0,99,0,"Interpolation",0, InterpolationSliderCallback };

Slider ChannelSlider;

Slider ImpulseLengthSlider;

Slider InputSensitivitySlider = {575, 665, 600, 65, 1, 200,0,"Input Sensitivity", 0, InputSensitivitySliderCallback };

typedef struct FFTArgs {
	int first_sample_index;
	int last_sample_index;
	int impulse_block_number;
	int num_callbacks_to_complete;
	int counter;
} FFTArgs;

FFTData *g_fftData_ptr; // Stores the Fourier-transforms of each block of the impulse

int g_block_length; // The length in frames of each audio buffer received by portaudio
int g_impulse_length; // The length in frames of the impulse
int g_num_blocks; // The number of equal-size blocks into which the impulse will be divided
int g_max_factor; // The highest power of 2 used to divide the impulse into blocks
int g_input_storage_buffer_length; // The length of the buffer used to store incoming audio from the mic
int g_output_storage_buffer1_length; // channel 1
int g_output_storage_buffer2_length; // channel 2
int g_end_sample; // The index of the last sample in g_storage_buffer
int g_counter = 0; // Keep track of how many callback cycles have passed

long g_block_duration_in_nanoseconds = (NANOSECONDS_IN_A_SECOND / SAMPLE_RATE)
				* MIN_FFT_BLOCK_SIZE;

Vector g_powerOf2Vector; // Stores the correct number of powers of 2 to make the block calculations work (lol)

pthread_t thread;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

audioData* g_impulse;

/*
 * This buffer is used to store INCOMING audio from the mic.
 */
float *g_input_storage_buffer;

/*
 * This buffer is used to store OUTGOING audio that has been processed.
 */
float *g_output_storage_buffer1; // channel 1
float *g_output_storage_buffer2; // channel 2

void setWindowRange();
void idleFunc();
void displayFunc();
void reshapeFunc(int width, int height);
void keyboardFunc(unsigned char, int, int);
void passiveMotionFunc(int, int);
void keyboardUpFunc(unsigned char, int, int);
void initialize_graphics();
void initialize_glut(int argc, char *argv[]);
void reloadImpulse();
float **getExponentialFitFromGraph(int num_impulse_blocks, int channel);
bool mouseCloseToTopLine();
bool mouseCloseToMidLine();
bool mouseCloseToBottomLine();
bool mouseCloseToALine();
bool mouseIsInDrawingArea();
bool mouseCloserToLeftBound();
bool mouseCloserToRightBound();
bool mouseBeganInImpulseHorizontalSelect();
GraphData *getValuesFromGraph(int current_channel);

void initializeGlobalParameters() {
	g_block_length = MIN_FFT_BLOCK_SIZE;
	g_num_blocks = g_impulse_length / g_block_length;
	g_max_factor = g_num_blocks / 4;
	g_input_storage_buffer_length = g_impulse_length / 4;
	g_output_storage_buffer1_length = g_input_storage_buffer_length * 2;
	g_output_storage_buffer2_length = g_input_storage_buffer_length * 2;
	g_end_sample = g_input_storage_buffer_length - 1;

	// Allocate memory for storage buffer, fill with 0s.
	g_input_storage_buffer = (float *) calloc(g_input_storage_buffer_length,
			sizeof(float));

	// Allocate memory for output storage buffer, fill with 0s.
	g_output_storage_buffer1 = (float *) calloc(g_output_storage_buffer1_length,
			sizeof(float));
	g_output_storage_buffer2 = (float *) calloc(g_output_storage_buffer2_length,
			sizeof(float));

}

int max(int a, int b) {
	return a >= b ? a : b;
}

int min(int a, int b) {
	return a >= b ? b : a;
}

void Font(void *font, char *text, int x, int y) {
	glRasterPos2i(x, y);
	while ( *text != '\0') {
		glutBitmapCharacter(font, *text);
		++text;
	}
}

// Is the mouse within the slider area?
int SliderClickTest(Slider* s, int x, int y) {
	if (s) {
		if(x > s->x_min - 10 && x < s->x_max + 10 && y < s->y + 10 && y > s->y - 10) {
			return 1;
		}
	}
	return 0;
}

/*----------------------------------------------------------------------------------------
 *	\brief	This function is used to see if a mouse click or event is within a button
 *			client area.
 *	\param	b	-	a pointer to the button to test
 *	\param	x	-	the x coord to test
 *	\param	y	-	the y-coord to test
 */
int ButtonClickTest(Button* b,int x,int y)
{
	if( b)
	{
		/*
		 *	If clicked within button area, then return true
		 */
		if( x > b->x      &&
				x < b->x+b->w &&
				y > b->y      &&
				y < b->y+b->h ) {
			return 1;
		}
	}

	/*
	 *	otherwise false.
	 */
	return 0;
}

/*----------------------------------------------------------------------------------------
 *	\brief	This function draws the specified button.
 *	\param	b	-	a pointer to the button to check.
 *	\param	x	-	the x location of the mouse cursor.
 *	\param	y	-	the y location of the mouse cursor.
 */
void ButtonRelease(Button *b,int x,int y)
{
	if(b)
	{
		/*
		 *	If the mouse button was pressed within the button area
		 *	as well as being released on the button.....
		 */
		if( ButtonClickTest(b,TheMouse.xpress,TheMouse.ypress) &&
				ButtonClickTest(b,x,y) )
		{
			/*
			 *	Then if a callback function has been set, call it.
			 */
			if (b->callbackFunction) {
				b->callbackFunction();
			}
		}

		/*
		 *	Set state back to zero.
		 */
		b->state = 0;
	}
}

void SliderRelease(Slider *s, int x, int y) {
	if (s) {
		if (SliderClickTest(s,TheMouse.xpress, TheMouse.ypress)) {
			if (s->callbackFunction) {
				s->callbackFunction();
			}
		}
	}
	s->state = 0;
}

/*----------------------------------------------------------------------------------------
 *	\brief	This function draws the specified button.
 *	\param	b	-	a pointer to the button to check.
 *	\param	x	-	the x location of the mouse cursor.
 *	\param	y	-	the y location of the mouse cursor.
 */
void ButtonPress(Button *b,int x,int y)
{
	if(b)
	{
		/*
		 *	if the mouse click was within the buttons client area,
		 *	set the state to true.
		 */
		if( ButtonClickTest(b,x,y) )
		{
			b->state = 1;
		}
	}
}

void SliderPress(Slider *s, int x, int y) {
	if (s) {
		if (SliderClickTest(s,x,y)) {
			s->state = 1;
			// Find how far along the slider the mouse is
			int range = s->x_max - s->x_min;
			float distance = (float) (x - s->x_min) / (float) range;
			int currentValue = ceil(distance * (s->max_val - s->min_val));
			if (currentValue < s->min_val) {
				currentValue = s->min_val;
			}
			if (currentValue > s->max_val) {
				currentValue = s->max_val;
			}

			s->x_pos = range*distance + s->x_min;

			if (s->x_pos <= s->x_min) {
				s->x_pos = s->x_min;
			}
			if (s->x_pos >= s->x_max) {
				s->x_pos = s->x_max;
			}
		}
	}
}

void SliderPassive(Slider *s, int x, int y) {

	if (s) {
		if (s->state == 1) {

			// Find how far along the slider the mouse is
			int range = s->x_max - s->x_min;
			float distance = (float) (x - s->x_min) / (float) range;
			int currentValue = ceil(distance * (s->max_val - s->min_val));
			if (currentValue < s->min_val) {
				currentValue = s->min_val;
			}
			if (currentValue > s->max_val) {
				currentValue = s->max_val;
			}

			s->x_pos = range*distance + s->x_min;

			if (s->x_pos <= s->x_min) {
				s->x_pos = s->x_min;
			}
			if (s->x_pos >= s->x_max) {
				s->x_pos = s->x_max;
			}



			//			printf("Slider selected, distance: %f, current value: %d\n", distance, currentValue);
		} else {
			//			printf("Slider not selected\n");
		}
	}

}

/*----------------------------------------------------------------------------------------
 *	\brief	This function draws the specified button.
 *	\param	b	-	a pointer to the button to check.
 *	\param	x	-	the x location of the mouse cursor.
 *	\param	y	-	the y location of the mouse cursor.
 */
void ButtonPassive(Button *b,int x,int y)
{
	if(b)
	{
		/*
		 *	if the mouse moved over the control
		 */
		if( ButtonClickTest(b,x,y) )
		{
			/*
			 *	If the cursor has just arrived over the control, set the highlighted flag
			 *	and force a redraw. The screen will not be redrawn again until the mouse
			 *	is no longer over this control
			 */
			if( b->highlighted == 0 ) {
				b->highlighted = 1;
				glutPostRedisplay();
			}
		}
		else

			/*
			 *	If the cursor is no longer over the control, then if the control
			 *	is highlighted (ie, the mouse has JUST moved off the control) then
			 *	we set the highlighting back to false, and force a redraw.
			 */
			if( b->highlighted == 1 )
			{
				b->highlighted = 0;
				glutPostRedisplay();
			}
	}
}

/*----------------------------------------------------------------------------------------
 *	\brief	This function is called whenever the mouse cursor is moved AND A BUTTON IS HELD.
 *	\param	x	-	the new x-coord of the mouse cursor.
 *	\param	y	-	the new y-coord of the mouse cursor.
 */
void MouseMotion(int x, int y)
{
	/*
	 *	Calculate how much the mouse actually moved
	 */
//	int dx = x - TheMouse.x;
//	int dy = y - TheMouse.y;

	/*
	 *	update the mouse position
	 */
	TheMouse.x = x;
	TheMouse.y = y;

	if (TheMouse.x != TheMouse.xpress && mouseBeganInImpulseHorizontalSelect()) {
		impulseHorizontalSelect->dragging = true;
	}

	if (impulseHorizontalSelect->dragging) {
		int new_left_bound = min(TheMouse.x, TheMouse.xpress);
		int new_right_bound = max(TheMouse.x, TheMouse.xpress);
		impulseHorizontalSelect->left_bound = new_left_bound;
		impulseHorizontalSelect->right_bound = new_right_bound;
	}

	/*
	 *	Check MyButton to see if we should highlight it cos the mouse is over it
	 */
	ButtonPassive(&RecomputeImpulseButton,x,y);
	ButtonPassive(&RandomizeButton,x,y);
	ButtonPassive(&ReverseButton,x,y);
	SliderPassive(&SmoothnessSlider,x,y);
	SliderPassive(&DryWetSlider,x,y);
	SliderPassive(&InterpolationSlider,x,y);
	SliderPassive(&ChannelSlider,x,y);
	SliderPassive(&ImpulseLengthSlider,x,y);
	SliderPassive(&InputSensitivitySlider,x,y);

	/*
	 *	Force a redraw of the screen
	 */
	glutPostRedisplay();
}

/*----------------------------------------------------------------------------------------
 *	\brief	This function is called whenever a mouse button is pressed or released
 *	\param	button	-	GLUT_LEFT_BUTTON, GLUT_RIGHT_BUTTON, or GLUT_MIDDLE_BUTTON
 *	\param	state	-	GLUT_UP or GLUT_DOWN depending on whether the mouse was released
 *						or pressed respectivly.
 *	\param	x		-	the x-coord of the mouse cursor.
 *	\param	y		-	the y-coord of the mouse cursor.
 */
void MouseButton(int button,int state,int x, int y)
{
	/*
	 *	update the mouse position
	 */
	TheMouse.x = x;
	TheMouse.y = y;

	/*
	 *	has the button been pressed or released?
	 */
	if (state == GLUT_DOWN)
	{
		/*
		 *	This holds the location of the first mouse click
		 */
		if ( !(TheMouse.lmb || TheMouse.mmb || TheMouse.rmb) ) {
			TheMouse.xpress = x;
			TheMouse.ypress = y;
		}

		if (mouseCloseToTopLine()) {
			impulseWindow->top_line_selected = true;
		}
		if (mouseCloseToMidLine()) {
			impulseWindow->mid_line_selected = true;
		}
		if (mouseCloseToBottomLine()) {
			impulseWindow->bottom_line_selected = true;
		}

		if (mouseIsInDrawingArea() && !mouseCloseToALine()) {
			g_drawing = true;
		}

		if (mouseCloserToLeftBound() && !impulseHorizontalSelect->dragging) {
			impulseHorizontalSelect->left_bound_selected = true;
		}
		if (mouseCloserToRightBound() && !impulseHorizontalSelect->dragging) {
			impulseHorizontalSelect->right_bound_selected = true;
		}

		/*
		 *	Which button was pressed?
		 */
		switch(button)
		{
		case GLUT_LEFT_BUTTON:
			TheMouse.lmb = 1;
			if (ButtonClickTest(&RecomputeImpulseButton,x,y)) {
				ButtonPress(&RecomputeImpulseButton,x,y);
			}
			if (ButtonClickTest(&RandomizeButton,x,y)) {
				ButtonPress(&RandomizeButton,x,y);
			}
			if (ButtonClickTest(&ReverseButton,x,y)) {
				ButtonPress(&ReverseButton,x,y);
			}
			if (SliderClickTest(&SmoothnessSlider,x,y)) {
				SliderPress(&SmoothnessSlider,x,y);
			}
			if (SliderClickTest(&DryWetSlider,x,y)) {
				SliderPress(&DryWetSlider,x,y);
			}
			if (SliderClickTest(&InterpolationSlider,x,y)) {
				SliderPress(&InterpolationSlider,x,y);
			}
			if (SliderClickTest(&ImpulseLengthSlider,x,y)) {
				SliderPress(&ImpulseLengthSlider,x,y);
			}
			if (SliderClickTest(&ChannelSlider,x,y)) {
				SliderPress(&ChannelSlider,x,y);
			}
			if (SliderClickTest(&InputSensitivitySlider,x,y)) {
				SliderPress(&InputSensitivitySlider,x,y);
			}
			break;

		case GLUT_MIDDLE_BUTTON:
			TheMouse.mmb = 1;
			break;
		case GLUT_RIGHT_BUTTON:
			TheMouse.rmb = 1;
			break;
		}
	}
	else
	{

		impulseWindow->top_line_selected = false;
		impulseWindow->mid_line_selected = false;
		impulseWindow->bottom_line_selected = false;

		impulseHorizontalSelect->left_bound_selected = false;
		impulseHorizontalSelect->right_bound_selected = false;

		if (impulseHorizontalSelect->dragging) {

			int new_left_bound = min(TheMouse.x, TheMouse.xpress);
			int new_right_bound = max(TheMouse.x, TheMouse.xpress);
			impulseHorizontalSelect->left_bound = new_left_bound;
			impulseHorizontalSelect->right_bound = new_right_bound;

			impulseHorizontalSelect->dragging = false;
		}



		g_drawing = false;

		/*
		 *	Which button was released?
		 */
		switch(button)
		{
		case GLUT_LEFT_BUTTON:
			TheMouse.lmb = 0;
			if (ButtonClickTest(&RecomputeImpulseButton,x,y)) {
				ButtonRelease(&RecomputeImpulseButton,x,y);
			}
			if (ButtonClickTest(&RandomizeButton,x,y)) {
				ButtonRelease(&RandomizeButton,x,y);
			}
			if (ButtonClickTest(&ReverseButton,x,y)) {
				ButtonRelease(&ReverseButton,x,y);
			}
			if (SliderClickTest(&SmoothnessSlider,TheMouse.xpress,TheMouse.ypress)) {
				SliderRelease(&SmoothnessSlider,x,y);
			}
			if (SliderClickTest(&DryWetSlider,TheMouse.xpress,TheMouse.ypress)) {
				SliderRelease(&DryWetSlider,x,y);
			}
			if (SliderClickTest(&InterpolationSlider,TheMouse.xpress,TheMouse.ypress)) {
				SliderRelease(&InterpolationSlider,x,y);
			}
			if (SliderClickTest(&ImpulseLengthSlider,TheMouse.xpress,TheMouse.ypress)) {
				SliderRelease(&ImpulseLengthSlider,x,y);
			}
			if (SliderClickTest(&ChannelSlider,TheMouse.xpress,TheMouse.ypress)) {
				SliderRelease(&ChannelSlider,x,y);
			}
			if (SliderClickTest(&InputSensitivitySlider,TheMouse.xpress,TheMouse.ypress)) {
				SliderRelease(&InputSensitivitySlider,x,y);
			}
			break;
		case GLUT_MIDDLE_BUTTON:
			TheMouse.mmb = 0;
			break;
		case GLUT_RIGHT_BUTTON:
			TheMouse.rmb = 0;
			break;
		}
	}

	//	printf("Mouse: x = %d, y = %d\n", TheMouse.x, TheMouse.y);

	/*
	 *	Force a redraw of the screen. If we later want interactions with the mouse
	 *	and the 3D scene, we will need to redraw the changes.
	 */
	glutPostRedisplay();
}

void SliderDraw(Slider *s) {
//	int fontx;
//	int fonty;

	float inc = (float) (s->x_max - s->x_min) / 100.0f;

	glLineWidth(2.0);
	glColor3f(0.0f, 0.0f, 1.0f);
	glBegin(GL_LINE_STRIP);
	for (int i=0; i<100; i++) {
		glColor3f((1.0f - (fabsf(50.0f-(float)i)/50.0f)) * 0.4f, (1.0f - (fabsf(50.0f-(float)i)/50.0f)) * 0.7f, 1.0f);
		glVertex2f(i*inc + s->x_min, s->y);
	}
	glEnd();
	glLineWidth(1.0);

	glColor3f(0.3f, 0.6f, 0.8f);
	glPointSize(19.0);
	glEnable( GL_POINT_SMOOTH );
	glBegin(GL_POINTS);
	glVertex2i(s->x_pos, s->y);
	glEnd();

	glColor3f(0.8f, 0.9f, 1.0f);

	glPointSize(16.0);
	glBegin(GL_POINTS);
	glVertex2i(s->x_pos, s->y);
	glDisable( GL_POINT_SMOOTH );
	glEnd();

	char buf[20];
	int range = s->x_max - s->x_min;
	float distance = (float) (s->x_pos - s->x_min) / (float) range;
	int currentValue = ceil(distance * (s->max_val - s->min_val));
	if (currentValue < s->min_val) {
		currentValue = s->min_val;
	}
	if (currentValue > s->max_val) {
		currentValue = s->max_val;
	}

//	int x_position;

	glColor3f(0,0,0);
	if (currentValue < 10) {
		snprintf(buf, 20, "%d", currentValue);
		Font(GLUT_BITMAP_HELVETICA_10, buf, s->x_pos-3, s->y+4);
	} else if (currentValue < 100) {
		snprintf(buf, 20, "%d", currentValue);
		Font(GLUT_BITMAP_HELVETICA_10, buf, s->x_pos-6, s->y+4);
	} else if (strcmp(s->label, "Length") == 0) {
		glColor3f(1,1,1);
		snprintf(buf, 20, "%f", (float)currentValue/(float)SAMPLE_RATE);
		Font(GLUT_BITMAP_HELVETICA_10, strcat(buf, " sec"), s->x_min + 40, s->y-15);
	} else if (strcmp(s->label, "Input Sensitivity") == 0 || strcmp(s->label, "Dry/Wet") == 0) {
		snprintf(buf, 20, "%d", currentValue);
		Font(GLUT_BITMAP_HELVETICA_10, buf, s->x_pos-8, s->y+4);
	}


	glColor3f(1,1,1);
	Font(GLUT_BITMAP_HELVETICA_10, s->label, s->x_min, s->y - 15);

	if(strcmp(s->label,"Interpolation") == 0) {
		if (InterpolationSlider.callbackFunction) {
			InterpolationSlider.callbackFunction();
		}
	}
	if(strcmp(s->label,"Smoothness") == 0) {
		if (SmoothnessSlider.callbackFunction) {
			SmoothnessSlider.callbackFunction();
		}
	}
	if(strcmp(s->label,"Dry/Wet") == 0) {
		if (DryWetSlider.callbackFunction) {
			DryWetSlider.callbackFunction();
		}
	}
	if(strcmp(s->label,"Channel") == 0) {
		if (ChannelSlider.callbackFunction) {
			ChannelSlider.callbackFunction();
		}
	}
	if(strcmp(s->label,"Input Sensitivity") == 0) {
	if (InputSensitivitySlider.callbackFunction) {
		InputSensitivitySlider.callbackFunction();
	}
}
}

void ButtonDraw(Button *b) {
	int fontx;
	int fonty;

	if (b) {
		if (b->highlighted) {
			glColor3f(0.7f, 0.7f, 0.8f);
		} else {
			glColor3f(0.6f, 0.6f, 0.6f);
		}
	}
	glBegin(GL_QUADS);
	glVertex2i(b->x, b->y);
	glVertex2i(b->x, b->y + b->h);
	glVertex2i(b->x + b->w, b->y + b->h);
	glVertex2i(b->x + b->w, b->y);
	glEnd();

	glLineWidth(3);

	if (b->state) {
		glColor3f(0.4f, 0.4f, 0.4f);
	} else {
		glColor3f(0.8f, 0.8f, 0.8f);
	}

	glBegin(GL_LINE_STRIP);
	glVertex2i(b->x + b->w, b->y);
	glVertex2i(b->x, b->y);
	glVertex2i(b->x, b->y + b->h);
	glEnd();

	if (b->state) {
		glColor3f(0.8f, 0.8f, 0.8f);
	} else {
		glColor3f(0.4f, 0.4f, 0.4f);
	}

	glBegin(GL_LINE_STRIP);
	glVertex2i(b->x, b->y + b->h);
	glVertex2i(b->x + b->w, b->y + b->h);
	glVertex2i(b->x + b->w, b->y);
	glEnd();

	glLineWidth(1);

	fontx = b->x + (b->w - glutBitmapLength(GLUT_BITMAP_HELVETICA_10, (const unsigned char *) b->label)) / 2;
	fonty = b->y + (b->h + 10) / 2;

	if (b->state) {
		fontx += 2;
		fonty += 2;
	}

	if (b->highlighted) {
		glColor3f(0,0,0);
		Font(GLUT_BITMAP_HELVETICA_10, b->label, fontx, fonty);
		fontx--;
		fonty--;
	}

	glColor3f(1,1,1);
	Font(GLUT_BITMAP_HELVETICA_10, b->label, fontx, fonty);

}

void RecomputeImpulseButtonCallback() {
	printf("reloading impulse...\n");
	g_changes_made++;
	g_changingImpulse = true;
	int i;
	for (i=0; i<g_output_storage_buffer1_length; i++) {
		g_output_storage_buffer1[i] = 0.0f;
		g_output_storage_buffer2[i] = 0.0f;
	}
	for (i=0; i<g_input_storage_buffer_length; i++) {
		g_input_storage_buffer[i] = 0.0f;
	}
	reloadImpulse();
	printf("impulse reloaded\n");
	g_r_pressed = true;
	for (i=0; i<g_output_storage_buffer1_length; i++) {
		g_output_storage_buffer1[i] = 0.0f;
		g_output_storage_buffer2[i] = 0.0f;
	}
	for (i=0; i<g_input_storage_buffer_length; i++) {
		g_input_storage_buffer[i] = 0.0f;
	}
	g_changingImpulse = false;
}

void ReverseButtonCallback() {
	GraphData *graphData = getValuesFromGraph(g_current_channel_view);

	if (!graphData) {
		return;
	}

	if (g_current_channel_view == LEFT) {
		for (int i=0; i<graphData->length; i++) {
			top_vals_left[graphData->first_index + i] = graphData->y_values[graphData->length - 1 - i];
		}
	} else if (g_current_channel_view == RIGHT) {
		for (int i=0; i<graphData->length; i++) {
			top_vals_right[graphData->first_index + i] = graphData->y_values[graphData->length - 1 - i];
		}
	}


	free(graphData->x_indices);
	free(graphData->y_values);
	free(graphData);
}

void RandomizeButtonCallback() {
	printf("Randomizing...\n");
	for (int i=0; i<HALF_FFT_SIZE; i++) {
		//		printf("top_vals_left[%d]: %f\n", i, top_vals_left[i]);
		float randomVal = ((rand() / (float) RAND_MAX) - 0.5)*0.05 + 1;
		//		printf("random: %f\n", randomVal);
		if (top_vals_left[i] * randomVal < 0.0f && top_vals_left[i] * randomVal > -6.0f) {
			top_vals_left[i] *= randomVal;
		}
		if (g_impulse->numChannels == STEREO) {
			if (top_vals_right[i] * randomVal < 0.0f && top_vals_left[i] * randomVal > -6.0f) {
				top_vals_right[i] *= randomVal;
			}
		}
	}
	RecomputeImpulseButtonCallback();
}

void SmoothnessSliderCallback() {
	//	printf("Smoothing...\n");
	// Find how far along the slider the mouse is
	int range = SmoothnessSlider.x_max - SmoothnessSlider.x_min;
	float distance = (float) (SmoothnessSlider.x_pos - SmoothnessSlider.x_min) / (float) range;
	int currentValue = ceil(distance * (SmoothnessSlider.max_val - SmoothnessSlider.min_val));
	if (currentValue < SmoothnessSlider.min_val) {
		currentValue = SmoothnessSlider.min_val;
	}
	if (currentValue > SmoothnessSlider.max_val) {
		currentValue = SmoothnessSlider.max_val;
	}

	SmoothnessSlider.current_val = currentValue;
	//	printf("The value is now %d\n", SmoothnessSlider.current_val);
	smooth_draw_amt = currentValue;
}

void ChannelSliderCallback() {
	if (abs(ChannelSlider.x_pos - ChannelSlider.x_min) < abs(ChannelSlider.x_pos - ChannelSlider.x_max)) {
		ChannelSlider.current_val = LEFT;
	} else {
		ChannelSlider.current_val = RIGHT;
	}
	g_current_channel_view = ChannelSlider.current_val;
}

void DryWetSliderCallback() {
	int range = DryWetSlider.x_max - DryWetSlider.x_min;
	float distance = (float) (DryWetSlider.x_pos - DryWetSlider.x_min) / (float) range;
	int currentValue = ceil(distance * (DryWetSlider.max_val - DryWetSlider.min_val));
	if (currentValue < DryWetSlider.min_val) {
		currentValue = DryWetSlider.min_val;
	}
	if (currentValue > DryWetSlider.max_val) {
		currentValue = DryWetSlider.max_val;
	}
	DryWetSlider.current_val = currentValue;
	g_dry_wet = currentValue;
	//	printf("Dry/Wet: %d\n", g_dry_wet);
}

void InterpolationSliderCallback() {
	int range = InterpolationSlider.x_max - InterpolationSlider.x_min;
	float distance = (float) (InterpolationSlider.x_pos - InterpolationSlider.x_min) / (float) range;
	int currentValue = ceil(distance * (InterpolationSlider.max_val - InterpolationSlider.min_val));
	if (currentValue < InterpolationSlider.min_val) {
		currentValue = InterpolationSlider.min_val;
	}
	if (currentValue > InterpolationSlider.max_val) {
		currentValue = InterpolationSlider.max_val;
	}

	InterpolationSlider.current_val = currentValue;
	//		printf("The value is now %d\n", InterpolationSlider.current_val);
	g_interpolation_amt = currentValue;
}

void InputSensitivitySliderCallback() {
	int range = InputSensitivitySlider.x_max - InputSensitivitySlider.x_min;
	float distance = (float) (InputSensitivitySlider.x_pos - InputSensitivitySlider.x_min) / (float) range;
	int currentValue = ceil(distance * (InputSensitivitySlider.max_val - InputSensitivitySlider.min_val));
	if (currentValue < InputSensitivitySlider.min_val) {
		currentValue = InputSensitivitySlider.min_val;
	}
	if (currentValue > InputSensitivitySlider.max_val) {
		currentValue = InputSensitivitySlider.max_val;
	}

	InputSensitivitySlider.current_val = currentValue;
	//		printf("The value is now %d\n", InterpolationSlider.current_val);
	g_input_sensitivity = currentValue;
}

void ImpulseLengthSliderCallback() {
	int range = ImpulseLengthSlider.x_max - ImpulseLengthSlider.x_min;
	float distance = (float) (ImpulseLengthSlider.x_pos - ImpulseLengthSlider.x_min) / (float) range;
	int currentValue = ceil(distance * (ImpulseLengthSlider.max_val - ImpulseLengthSlider.min_val));
	if (currentValue < ImpulseLengthSlider.min_val) {
		currentValue = ImpulseLengthSlider.min_val;
	}
	if (currentValue > ImpulseLengthSlider.max_val) {
		currentValue = ImpulseLengthSlider.max_val;
	}

	ImpulseLengthSlider.current_val = currentValue;
	printf("The impulse length is now %f seconds\n", (float) ImpulseLengthSlider.current_val/(float)SAMPLE_RATE);
	g_impulse_num_frames = currentValue;
	RecomputeImpulseButtonCallback();
}

//-----------------------------------------------------------------------------
// Name: initialize_glut( )
// Desc: Initializes Glut with the global vars
//-----------------------------------------------------------------------------
void initialize_glut(int argc, char *argv[]) {
	// initialize GLUT
	glutInit(&argc, argv);
	// double buffer, use rgb color, enable depth buffer
	glutInitDisplayMode( GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
	// initialize the window size
	glutInitWindowSize(g_width, g_height);
	// set the window postion
	glutInitWindowPosition(100, 100);
	// create the window
	glutCreateWindow("Synthesized Convolution Reverb");
	// full screen
	if (g_fullscreen)
		glutFullScreen();

	// set the idle function - called when idle
	glutIdleFunc(idleFunc);
	// set the display function - called when redrawing
	glutDisplayFunc(displayFunc);
	// set the reshape function - called when client area changes
	glutReshapeFunc(reshapeFunc);
	// set the keyboard function - called on keyboard events
	glutKeyboardFunc(keyboardFunc);
	// get mouse position
	glutPassiveMotionFunc(passiveMotionFunc);

	glutKeyboardUpFunc(keyboardUpFunc);

	glutMouseFunc(MouseButton);

	glutMotionFunc(MouseMotion);

	// do our own initialization
	initialize_graphics();
}

void idleFunc() {
	glutPostRedisplay();
}

void keyboardFunc(unsigned char key, int x, int y) {
	switch (key) {
	case 'f':
		if (!g_fullscreen) {
			g_last_width = g_width;
			g_last_height = g_height;
			glutFullScreen();
		} else {
			glutReshapeWindow(g_last_width, g_last_height);
		}
		g_fullscreen = !g_fullscreen;
		break;
	case 'r':
		if (!g_r_pressed) {
			printf("reloading impulse...\n");
			g_changes_made++;
			g_changingImpulse = true;
			int i;
			for (i=0; i<g_output_storage_buffer1_length; i++) {
				g_output_storage_buffer1[i] = 0.0f;
				g_output_storage_buffer2[i] = 0.0f;
			}
			for (i=0; i<g_input_storage_buffer_length; i++) {
				g_input_storage_buffer[i] = 0.0f;
			}
			//		getExponentialFitFromGraph(g_impulse->numFrames / FFT_SIZE);
			reloadImpulse();
			printf("impulse reloaded\n");
			g_r_pressed = true;
			for (i=0; i<g_output_storage_buffer1_length; i++) {
				g_output_storage_buffer1[i] = 0.0f;
				g_output_storage_buffer2[i] = 0.0f;
			}
			for (i=0; i<g_input_storage_buffer_length; i++) {
				g_input_storage_buffer[i] = 0.0f;
			}
			g_changingImpulse = false;
		}
		break;
	case 'q':
		exit(0);
		break;
	case 'p':
		if (smooth_draw && smooth_draw_amt < HALF_FFT_SIZE / 8) {
			smooth_draw_amt++;
			printf("smooth_draw_amt: %d\n", smooth_draw_amt);
		}
		break;
	case 'o':
		if (smooth_draw && smooth_draw_amt > 3) {
			smooth_draw_amt--;
			printf("smooth_draw_amt: %d\n", smooth_draw_amt);
		}
		break;

	case 'a':
		a_pressed = true;
		//		printf("'a' has been pressed.\n");
		break;
	case 'z':
		z_pressed = true;
		break;
	}
	glutPostRedisplay();
}

void passiveMotionFunc(int x, int y) {
	g_mouse_x = ((float) HALF_FFT_SIZE / 576) * (float) x
			- ((float) HALF_FFT_SIZE * 111 / 576);

	g_mouse_y = -4.14 + 8.28 * ((float) (g_height - y) / (float) g_height);

	/*
	 *	Calculate how much the mouse actually moved
	 */
//	int dx = x - TheMouse.x;
//	int dy = y - TheMouse.y;

	/*
	 *	update the mouse position
	 */
	TheMouse.x = x;
	TheMouse.y = y;

	/*
	 *	Check MyButton to see if we should highlight it cos the mouse is over it
	 */
	ButtonPassive(&RecomputeImpulseButton,x,y);
	ButtonPassive(&RandomizeButton,x,y);
	ButtonPassive(&ReverseButton,x,y);
	//	SliderPassive(&SmoothnessSlider,x,y);

	/*
	 *	Note that I'm not using a glutPostRedisplay() call here. The passive motion function
	 *	is called at a very high frequency. We really don't want much processing to occur here.
	 *	Redrawing the screen every time the mouse moves is a bit excessive. Later on we
	 *	will look at a way to solve this problem and force a redraw only when needed.
	 */

	//	printf("x: %f, y: %f\n", g_mouse_x, g_mouse_y);
}

void keyboardUpFunc(unsigned char key, int x, int y) {
	switch (key) {
	case 'a':
		a_pressed = false;
		//		printf("'a' has been released.\n");
		break;
	case 'r':
		g_r_pressed = false;
		break;
	case 'z':
		z_pressed = false;
		break;
	}
	glutPostRedisplay();
}

void reshapeFunc(int w, int h) {
	// save the new window size
	g_width = w;
	g_height = h;
	setWindowRange();
	// map the view port to the client area
	glViewport(0, 0, w, h);
	// set the matrix mode to project
	glMatrixMode( GL_PROJECTION);
	// load the identity matrix
	glLoadIdentity();
	// create the viewing frustum
	//gluPerspective( 45.0, (GLfloat) w / (GLfloat) h, .05, 50.0 );
	gluPerspective(45.0, (GLfloat) w / (GLfloat) h, 1.0, 1000.0);
	// set the matrix mode to modelview
	glMatrixMode( GL_MODELVIEW);
	// load the identity matrix
	glLoadIdentity();

	// position the view point
	//  void gluLookAt( GLdouble eyeX,
	//                 GLdouble eyeY,
	//                 GLdouble eyeZ,
	//                 GLdouble centerX,
	//                 GLdouble centerY,
	//                 GLdouble centerZ,
	//                 GLdouble upX,
	//                 GLdouble upY,
	//                 GLdouble upZ )

	gluLookAt(0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
	/*gluLookAt( 0.0f, 3.5f * sin( 0.0f ), 3.5f * cos( 0.0f ),
	 0.0f, 0.0f, 0.0f,
	 0.0f, 1.0f , 0.0f );*/
}

//-----------------------------------------------------------------------------
// Name: initialize_graphics( )
// Desc: sets initial OpenGL states and initializes any application data
//-----------------------------------------------------------------------------
void initialize_graphics() {

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);					// Black Background
	// set the shading model to 'smooth'
	glShadeModel( GL_SMOOTH);
	// enable depth
	glEnable( GL_DEPTH_TEST);
	// set the front faces of polygons
	glFrontFace( GL_CCW);
	// set fill mode
	glPolygonMode( GL_FRONT_AND_BACK, g_fillmode);
	// enable lighting
	glEnable( GL_LIGHTING);
	// enable lighting for front
	glLightModeli( GL_FRONT_AND_BACK, GL_TRUE);
	// material have diffuse and ambient lighting
	glColorMaterial( GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
	// enable color
	glEnable( GL_COLOR_MATERIAL);
	// normalize (for scaling)
	glEnable( GL_NORMALIZE);
	// line width
	glLineWidth(g_linewidth);

	// enable light 0
	glEnable( GL_LIGHT0);

	glEnable( GL_LIGHT1);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

}

bool mouseBeganInImpulseHorizontalSelect() {
	return (TheMouse.ypress >= impulseHorizontalSelect->top_left.y &&
			TheMouse.ypress <= impulseHorizontalSelect->bottom_left.y &&
			TheMouse.xpress >= impulseHorizontalSelect->top_left.x &&
			TheMouse.xpress <= impulseHorizontalSelect->top_right.x);
}

bool mouseInImpulseHorizontalSelect() {
	return (TheMouse.y >= impulseHorizontalSelect->top_left.y &&
			TheMouse.y <= impulseHorizontalSelect->bottom_left.y &&
			TheMouse.x >= impulseHorizontalSelect->top_left.x &&
			TheMouse.x <= impulseHorizontalSelect->top_right.x);
}

bool mouseCloserToLeftBound() {
	if (!mouseInImpulseHorizontalSelect() || impulseHorizontalSelect->dragging) {
		return false;
	}
	return (abs(TheMouse.x - impulseHorizontalSelect->left_bound) < abs(TheMouse.x - impulseHorizontalSelect->right_bound));
}

bool mouseCloserToRightBound() {
	if (!mouseInImpulseHorizontalSelect() || impulseHorizontalSelect->dragging) {
		return false;
	}
	return (abs(TheMouse.x - impulseHorizontalSelect->left_bound) >= abs(TheMouse.x - impulseHorizontalSelect->right_bound));
}

bool mouseCloseToTopLine() {

	if (g_drawing) {
		return false;
	}
	if (impulseWindow->top_line_selected) {
		return true;
	}
	if (impulseWindow->mid_line_selected || impulseWindow->bottom_line_selected) {
		return false;
	}
	if (abs(TheMouse.y - impulseWindow->top_line) < 20) {
		return true;
	} else {
		return false;
	}

}

bool mouseCloseToMidLine() {

	if (g_drawing) {
		return false;
	}
	if (impulseWindow->mid_line_selected) {
		return true;
	}
	if (impulseWindow->top_line_selected || impulseWindow->bottom_line_selected) {
		return false;
	}
	if (abs(TheMouse.y - impulseWindow->mid_line) < 20) {
		return true;
	} else {
		return false;
	}

}

bool mouseCloseToBottomLine() {

	if (g_drawing) {
		return false;
	}
	if (impulseWindow->bottom_line_selected) {
		return true;
	}
	if (impulseWindow->mid_line_selected || impulseWindow->top_line_selected) {
		return false;
	}
	if (abs(TheMouse.y - impulseWindow->bottom_line) < 20) {
		return true;
	} else {
		return false;
	}

}

bool mouseCloseToALine() {
	return (mouseCloseToBottomLine() || mouseCloseToMidLine() || mouseCloseToTopLine());
}

bool mouseBeganInDrawingArea() {
	return (TheMouse.ypress > impulseWindow->margin_top - impulseWindow->edge_margin && TheMouse.ypress < impulseWindow->margin_bottom + impulseWindow->edge_margin && TheMouse.xpress > impulseWindow->margin_left && TheMouse.xpress < impulseWindow->margin_right);
}

bool mouseIsInDrawingArea() {
	return (TheMouse.y > impulseWindow->margin_top - impulseWindow->edge_margin && TheMouse.y < impulseWindow->margin_bottom + impulseWindow->edge_margin && TheMouse.x > impulseWindow->margin_left && TheMouse.x < impulseWindow->margin_right);
}

bool noElementsAreSelected() {
	if (impulseWindow->bottom_line_selected || impulseWindow->mid_line_selected || impulseWindow->top_line_selected || InputSensitivitySlider.state == 1 || DryWetSlider.state == 1 || ChannelSlider.state == 1 || SmoothnessSlider.state == 1 || InterpolationSlider.state == 1 || ImpulseLengthSlider.state == 1 || RecomputeImpulseButton.state == 1 || RandomizeButton.state == 1 || ReverseButton.state == 1) {
		return false;
	}
	return true;
}

void displayFunc() {
	int i, j;

//	float x_location;

	//	while (!g_ready) {
	//		usleep(1000);
	//	}
	//	g_ready = false;

	//	glMatrixMode(GL_MODELVIEW);
	//	glLoadIdentity();

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_LIGHTING);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	// draw view here
	// If the mouse is in the drawing area
	if (mouseBeganInDrawingArea() && noElementsAreSelected()) {

		int index = 0;

		for (int i=0; i<HALF_FFT_SIZE; i++) {
			if ((TheMouse.x - impulseWindow->margin_left) > x_values[i]) {
				index = i;
				g_current_index = i;
			}
		}

		if (g_current_channel_view == LEFT) {
			// If draw button is pressed
			if (TheMouse.lmb == 1) {

				//			printf("index: %d\n", index);

				if (index < 0) {
					index = 0;
				}
				if (index > HALF_FFT_SIZE - 1) {
					index = HALF_FFT_SIZE - 1;
				}

				// Set the value of the top_vals array using the y-value of the mouse position
				//			printf("Mouse: %d, %d\n", TheMouse.x, TheMouse.y);
				float newVal = 6 * ((float) impulseWindow->margin_top - (float) TheMouse.y) / ((float) impulseWindow->margin_bottom - (float) impulseWindow->margin_top);
				//				printf("NewVal: %f\n", newVal);
				if (newVal > 0.0f) {
					newVal = 0.0f;
				}
				if (newVal < -5.99f) {
					newVal = -5.99f;
				}
				top_vals_left[index] = newVal;

				// If smooth draw is activated
				if (smooth_draw) {
					// If the mouse is in the center of the drawing area
					if (index - smooth_draw_amt >= 0 && index + smooth_draw_amt < HALF_FFT_SIZE) {
						int index_to_the_left = index - smooth_draw_amt;
						int index_to_the_right = index + smooth_draw_amt;
						float val_to_the_left = top_vals_left[index_to_the_left];
						float val_to_the_right = top_vals_left[index_to_the_right];
						float inc_to_the_left = (newVal - val_to_the_left)/(float) smooth_draw_amt;
						float inc_to_the_right = (newVal - val_to_the_right)/(float) smooth_draw_amt;

						for (int i=1; i<smooth_draw_amt; i++) {
							// Values to the left
							top_vals_left[index - i] = newVal - i*inc_to_the_left;
							// Values to the right
							top_vals_left[index + i] = newVal - i*inc_to_the_right;
						}
					}
					// If the mouse is near the left of the drawing area
					if (index - smooth_draw_amt < 0) {
						int index_to_the_left = 0;
						int index_to_the_right = index + smooth_draw_amt;
						float val_to_the_left = top_vals_left[index_to_the_left];
						float val_to_the_right = top_vals_left[index_to_the_right];
						float inc_to_the_left = (newVal - val_to_the_left)/(float) (index - index_to_the_left);
						float inc_to_the_right = (newVal - val_to_the_right)/(float) smooth_draw_amt;
						for (int i=1; i < (index - index_to_the_left); i++) {
							top_vals_left[index - i] = newVal - i*inc_to_the_left;
						}
						for (int i=1; i < smooth_draw_amt; i++) {
							top_vals_left[index + i] = newVal - i*inc_to_the_right;
						}
					}
					// If the mouse is near the right of the drawing area
					if (index + smooth_draw_amt >= HALF_FFT_SIZE) {
						int index_to_the_left = index - smooth_draw_amt;
						int index_to_the_right = HALF_FFT_SIZE - 1;
						float val_to_the_left = top_vals_left[index_to_the_left];
						float val_to_the_right = top_vals_left[index_to_the_right];
						float inc_to_the_left = (newVal - val_to_the_left)/(float) smooth_draw_amt;
						float inc_to_the_right = (newVal - val_to_the_right)/(float) (index_to_the_right - index);
						for (int i=1; i < smooth_draw_amt ; i++) {
							top_vals_left[index - i] = newVal - i*inc_to_the_left;
						}
						for (int i=1; i < (index_to_the_right - index); i++) {
							top_vals_left[index + i] = newVal - i*inc_to_the_right;
						}
					}
				}

			}
		} else if (g_current_channel_view == RIGHT && g_impulse->numChannels == STEREO) {
			// If draw button is pressed
			if (TheMouse.lmb == 1) {

				//			printf("index: %d\n", index);

				if (index < 0) {
					index = 0;
				}
				if (index > HALF_FFT_SIZE - 1) {
					index = HALF_FFT_SIZE - 1;
				}

				// Set the value of the top_vals array using the y-value of the mouse position
				//			printf("Mouse: %d, %d\n", TheMouse.x, TheMouse.y);
				float newVal = 6 * ((float) impulseWindow->margin_top - (float) TheMouse.y) / ((float) impulseWindow->margin_bottom - (float) impulseWindow->margin_top);
				//			printf("NewVal: %f\n", newVal);
				if (newVal > 0.0f) {
					newVal = 0.0f;
				}
				if (newVal < -5.99f) {
					newVal = -5.99f;
				}
				top_vals_right[index] = newVal;

				// If smooth draw is activated
				if (smooth_draw) {
					// If the mouse is in the center of the drawing area
					if (index - smooth_draw_amt >= 0 && index + smooth_draw_amt < HALF_FFT_SIZE) {
						int index_to_the_left = index - smooth_draw_amt;
						int index_to_the_right = index + smooth_draw_amt;
						float val_to_the_left = top_vals_right[index_to_the_left];
						float val_to_the_right = top_vals_right[index_to_the_right];
						float inc_to_the_left = (newVal - val_to_the_left)/(float) smooth_draw_amt;
						float inc_to_the_right = (newVal - val_to_the_right)/(float) smooth_draw_amt;

						for (int i=1; i<smooth_draw_amt; i++) {
							// Values to the left
							top_vals_right[index - i] = newVal - i*inc_to_the_left;
							// Values to the right
							top_vals_right[index + i] = newVal - i*inc_to_the_right;
						}
					}
					// If the mouse is near the left of the drawing area
					if (index - smooth_draw_amt < 0) {
						int index_to_the_left = 0;
						int index_to_the_right = index + smooth_draw_amt;
						float val_to_the_left = top_vals_right[index_to_the_left];
						float val_to_the_right = top_vals_right[index_to_the_right];
						float inc_to_the_left = (newVal - val_to_the_left)/(float) (index - index_to_the_left);
						float inc_to_the_right = (newVal - val_to_the_right)/(float) smooth_draw_amt;
						for (int i=1; i < (index - index_to_the_left) ; i++) {
							top_vals_right[index - i] = newVal - i*inc_to_the_left;
						}
						for (int i=1; i < smooth_draw_amt; i++) {
							top_vals_right[index + i] = newVal - i*inc_to_the_right;
						}
					}
					// If the mouse is near the right of the drawing area
					if (index + smooth_draw_amt >= HALF_FFT_SIZE) {
						int index_to_the_left = index - smooth_draw_amt;
						int index_to_the_right = HALF_FFT_SIZE - 1;
						float val_to_the_left = top_vals_right[index_to_the_left];
						float val_to_the_right = top_vals_right[index_to_the_right];
						float inc_to_the_left = (newVal - val_to_the_left)/(float) smooth_draw_amt;
						float inc_to_the_right = (newVal - val_to_the_right)/(float) (index_to_the_right - index);
						for (int i=1; i < smooth_draw_amt ; i++) {
							top_vals_right[index - i] = newVal - i*inc_to_the_left;
						}
						for (int i=1; i < (index_to_the_right - index); i++) {
							top_vals_right[index + i] = newVal - i*inc_to_the_right;
						}
					}
				}
			}
		}
	}

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0,g_width, g_height, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	ButtonDraw(&RecomputeImpulseButton);
	ButtonDraw(&RandomizeButton);
	ButtonDraw(&ReverseButton);
	SliderDraw(&SmoothnessSlider);
	SliderDraw(&DryWetSlider);
	SliderDraw(&InterpolationSlider);
	SliderDraw(&ChannelSlider);
	SliderDraw(&ImpulseLengthSlider);
	SliderDraw(&InputSensitivitySlider);

	glBegin(GL_QUADS);
	glColor3f(0.1f,0.1f,0.2f);
	glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->margin_top - impulseWindow->edge_margin, 0.0f);
	glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->margin_bottom + impulseWindow->edge_margin, 0.0f);
	glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->margin_bottom + impulseWindow->edge_margin, 0.0f);
	glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->margin_top - impulseWindow->edge_margin, 0.0f);
	glEnd();

	float line_opacity_when_not_selected = 0.2f;

	if (impulseWindow->top_line_selected || mouseCloseToTopLine()) {
		glLineWidth(3);
		glColor4f(0.5f, 0.2f, 0.7f, 1.0f);
	} else {
		glLineWidth(1);
		glColor4f(0.5f, 0.2f, 0.7f, line_opacity_when_not_selected);
	}

	if (impulseWindow->top_line_selected) {
		impulseWindow->top_line = TheMouse.y;
		if (impulseWindow->top_line < impulseWindow->margin_top) {
			impulseWindow->top_line = impulseWindow->margin_top;
		}
		if (impulseWindow->top_line > impulseWindow->mid_line - 20) {
			impulseWindow->top_line = impulseWindow->mid_line - 20;
		}
	}

	// Draw top line
	glBegin(GL_LINE_STRIP);
	glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->top_line, 0.0f);
	glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->top_line, 0.0f);
	glEnd();

	if (impulseWindow->mid_line_selected || mouseCloseToMidLine()) {
		glLineWidth(3);
		glColor4f(0.5f, 0.4f, 0.5f, 1.0f);
	} else {
		glLineWidth(1);
		glColor4f(0.5f, 0.4f, 0.5f, line_opacity_when_not_selected);
	}

	if (impulseWindow->mid_line_selected) {
		impulseWindow->mid_line = TheMouse.y;
		if (impulseWindow->mid_line < impulseWindow->top_line + 20) {
			impulseWindow->mid_line = impulseWindow->top_line + 20;
		}
		if (impulseWindow->mid_line > impulseWindow->bottom_line - 20) {
			impulseWindow->mid_line = impulseWindow->bottom_line - 20;
		}
	}

	// Draw mid line
	glBegin(GL_LINE_STRIP);
	glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->mid_line, 0.0f);
	glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->mid_line, 0.0f);
	glEnd();

	if (impulseWindow->bottom_line_selected || mouseCloseToBottomLine()) {
		glLineWidth(3);
		glColor4f(0.5f, 0.6f, 0.3f, 1.0f);
	} else {
		glLineWidth(1);
		glColor4f(0.5f, 0.6f, 0.3f, line_opacity_when_not_selected);
	}

	if (impulseWindow->bottom_line_selected) {
		impulseWindow->bottom_line = TheMouse.y;
		if (impulseWindow->bottom_line < impulseWindow->mid_line + 20) {
			impulseWindow->bottom_line = impulseWindow->mid_line + 20;
		}
		if (impulseWindow->bottom_line > impulseWindow->margin_bottom) {
			impulseWindow->bottom_line = impulseWindow->margin_bottom;
		}
	}

	// Draw bottom line
	glBegin(GL_LINE_STRIP);
	glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->bottom_line, 0.0f);
	glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->bottom_line, 0.0f);
	glEnd();

	glLineWidth(3);

	// Draw horizontal select
	glBegin(GL_QUADS);
	glColor3f(0.2f, 0.35f, 0.5f);
	glVertex3f(impulseHorizontalSelect->top_left.x, impulseHorizontalSelect->top_left.y, 0.0f);
	glVertex3f(impulseHorizontalSelect->top_right.x, impulseHorizontalSelect->top_right.y, 0.0f);
	glVertex3f(impulseHorizontalSelect->bottom_right.x, impulseHorizontalSelect->bottom_right.y, 0.0f);
	glVertex3f(impulseHorizontalSelect->bottom_left.x, impulseHorizontalSelect->bottom_left.y, 0.0f);
	glEnd();

	// Draw left bound
	if (mouseCloserToLeftBound()) {
		glLineWidth(3);
	} else {
		glLineWidth(1);
	}

	if (impulseHorizontalSelect->left_bound_selected && !impulseHorizontalSelect->dragging) {
		impulseHorizontalSelect->left_bound = TheMouse.x;
	}
	if (impulseHorizontalSelect->left_bound < impulseHorizontalSelect->top_left.x) {
		impulseHorizontalSelect->left_bound = impulseHorizontalSelect->top_left.x;
	}
	if (impulseHorizontalSelect->left_bound > impulseHorizontalSelect->right_bound - 1) {
		impulseHorizontalSelect->left_bound = impulseHorizontalSelect->right_bound - 1;
	}

	glBegin(GL_LINE_STRIP);
	glColor3f(1,1,1);
	glVertex3f(impulseHorizontalSelect->left_bound, impulseHorizontalSelect->top_left.y, 0.0f);
	glVertex3f(impulseHorizontalSelect->left_bound, impulseHorizontalSelect->bottom_left.y, 0.0f);
	glEnd();

	// Draw right bound
	if (mouseCloserToRightBound()) {
		glLineWidth(3);
	} else {
		glLineWidth(1);
	}

	if (impulseHorizontalSelect->right_bound_selected && !impulseHorizontalSelect->dragging) {
		impulseHorizontalSelect->right_bound = TheMouse.x;
	}
	if (impulseHorizontalSelect->right_bound > impulseHorizontalSelect->top_right.x) {
		impulseHorizontalSelect->right_bound = impulseHorizontalSelect->top_right.x;
	}

	if (impulseHorizontalSelect->right_bound < impulseHorizontalSelect->left_bound + 1) {
		impulseHorizontalSelect->right_bound = impulseHorizontalSelect->left_bound + 1;
	}

	glBegin(GL_LINE_STRIP);
	glColor3f(1,1,1);
	glVertex3f(impulseHorizontalSelect->right_bound, impulseHorizontalSelect->top_left.y, 0.0f);
	glVertex3f(impulseHorizontalSelect->right_bound, impulseHorizontalSelect->bottom_left.y, 0.0f);
	glEnd();

	// Draw dragged area, if applicable
	if (impulseHorizontalSelect->dragging) {
		glLineWidth(5);
		glBegin(GL_LINE_STRIP);
		glColor3f(0,0,0);
		glVertex3f(TheMouse.x, (impulseHorizontalSelect->bottom_left.y - impulseHorizontalSelect->top_left.y)/2 + impulseHorizontalSelect->top_left.y, 0.0f);
		glVertex3f(TheMouse.xpress, (impulseHorizontalSelect->bottom_left.y - impulseHorizontalSelect->top_left.y)/2 + impulseHorizontalSelect->top_left.y, 0.0f);
		glEnd();
	}

	float opacity = 0.02;

	if (mouseInImpulseHorizontalSelect() || impulseHorizontalSelect->dragging || mouseCloseToTopLine() || mouseCloseToBottomLine()) {
		opacity = 0.15;
	}

	// Draw selected area
	glBegin(GL_QUADS);
	glColor4f(1,1,1,opacity);
	glVertex3f(impulseHorizontalSelect->left_bound, impulseWindow->top_line, 0.0f);
	glVertex3f(impulseHorizontalSelect->right_bound, impulseWindow->top_line, 0.0f);
	glVertex3f(impulseHorizontalSelect->right_bound, impulseWindow->bottom_line, 0.0f);
	glVertex3f(impulseHorizontalSelect->left_bound, impulseWindow->bottom_line, 0.0f);
	glEnd();
	glLineWidth(3);


	//TODO: manipulate values that are in the selected range
//	for (int i=0; i<HALF_FFT_SIZE; i++) {
//		if (impulseWindow->margin_left + x_values[i] > impulseHorizontalSelect->left_bound && impulseWindow->margin_left + x_values[i] < impulseHorizontalSelect->right_bound) {
//			printf("x_values[%d] is in range\n", i);
//		}
//	}

	// Draw y axis
	glPushMatrix();
	{
		glBegin(GL_LINE_STRIP);
		glColor3f(0.2f, 0.35f, 0.5f);
		glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->margin_top - impulseWindow->edge_margin, 0.0f);
		glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->margin_bottom + impulseWindow->edge_margin, 0.0f);
		glEnd();
		glBegin(GL_LINE_STRIP);
		glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->margin_top - impulseWindow->edge_margin, 0.0f);
		glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->margin_bottom + impulseWindow->edge_margin, 0.0f);
		glEnd();
	}
	glPopMatrix();

	float divisor = HALF_FFT_SIZE;

	float index_x_inc = (float) (impulseWindow->margin_right - impulseWindow->margin_left) / divisor;

	// Draw x axis
	glPushMatrix();
	{
		glBegin(GL_LINE_STRIP);
		glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->margin_bottom + impulseWindow->edge_margin, 0.0f);
		glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->margin_bottom + impulseWindow->edge_margin , 0.0f);
		glEnd();
		glBegin(GL_LINE_STRIP);
		glVertex3f(impulseWindow->margin_left - impulseWindow->edge_margin, impulseWindow->margin_top - impulseWindow->edge_margin, 0.0f);
		glVertex3f(impulseWindow->margin_right + impulseWindow->edge_margin, impulseWindow->margin_top - impulseWindow->edge_margin , 0.0f);
		glEnd();
	}
	glPopMatrix();

	glLineWidth(1);

	if (g_current_channel_view == LEFT) {
		// Draw impulse
		glPushMatrix();
		{
			for (i = 0; i < HALF_FFT_SIZE; i++) {

				float top_value = impulseWindow->margin_top - top_vals_left[i]*((float) impulseWindow->margin_bottom - impulseWindow->margin_top)/6;
				float inc = ((float)impulseWindow->margin_bottom - top_value) / 100;

				if (i > 0) {
					float top_val_to_the_left = impulseWindow->margin_top - top_vals_left[i-1]*((float) impulseWindow->margin_bottom - impulseWindow->margin_top)/6;

					float y_inc = (top_value - top_val_to_the_left) / g_interpolation_amt;
					float x_inc = (x_values[i] - x_values[i-1])/g_interpolation_amt;

					for (int k=0; k<g_interpolation_amt; k++) {
						glBegin(GL_LINE_STRIP);
						for (int l=0; l<100; l++) {
							if (abs(g_current_index - i) < smooth_draw_amt) {
								glColor3f((float) (100 - l) * 0.8 / 100, (float) (100 - l) * 0.3 / 100,
										(float) (100 - l) * 0.3 / 100);
							} else {
								glColor3f((float) (100 - l) * 0.6 / 100, (float) (100 - l) * 0.5 / 100,
										(float) (100 - l) * 0.5 / 100);
							}
							float height = top_val_to_the_left + k*y_inc;
							float height_inc = ((float) impulseWindow->margin_bottom - height) / 100;
							glVertex3f(impulseWindow->margin_left + x_values[i-1] + x_inc*k, height + height_inc*l, 0.0f);
						}
						glEnd();
					}

				}

				if (g_current_index == i && mouseIsInDrawingArea() && noElementsAreSelected() && !mouseCloseToALine()) {
					glLineWidth(6);
				} else if (abs(g_current_index - i) < smooth_draw_amt && !mouseCloseToALine() && mouseIsInDrawingArea()) {
					glLineWidth(3);
				} else {
					glLineWidth(1);
				}

				glBegin(GL_LINE_STRIP);
				for (j = 0; j < 100; j++) {
					if (g_current_index == i && mouseIsInDrawingArea() && noElementsAreSelected() && !mouseCloseToALine()) {
						glColor3f((float) (100 - j) * 1.0 / 100, (float) (100 - j) * 0.1 / 100,
								(float) (100 - j) * 0.1 / 100);
					} else if (abs(g_current_index - i) < smooth_draw_amt && !mouseCloseToALine() && mouseIsInDrawingArea()) {
						glColor3f((float) (100 - j) * 0.8 / 100, (float) (100 - j) * 0.3 / 100,
								(float) (100 - j) * 0.3 / 100);
					} else {
						glColor3f((float) (100 - j) * 0.6 / 100, (float) (100 - j) * 0.5 / 100,
								(float) (100 - j) * 0.5 / 100);
					}
					float x_value = index_x_inc*log(i+1)*(float)HALF_FFT_SIZE/log((float)HALF_FFT_SIZE);
					x_values[i] = x_value;
					glVertex3f(impulseWindow->margin_left + x_value, top_value + (float) j * inc, 0.0f);

				}
				glEnd();
				glLineWidth(1);
			}
		}
		glPopMatrix();
	} else if (g_current_channel_view == RIGHT && g_impulse->numChannels == STEREO) {
		// Draw impulse
		glPushMatrix();
		{
			for (i = 0; i < HALF_FFT_SIZE; i++) {

				float top_value = impulseWindow->margin_top - top_vals_right[i]*((float) impulseWindow->margin_bottom - impulseWindow->margin_top)/6;
				float inc = ((float)impulseWindow->margin_bottom - top_value) / 100;

				if (i > 0) {
					float top_val_to_the_left = impulseWindow->margin_top - top_vals_right[i-1]*((float) impulseWindow->margin_bottom - impulseWindow->margin_top)/6;

					float y_inc = (top_value - top_val_to_the_left) / g_interpolation_amt;
					float x_inc = (x_values[i] - x_values[i-1])/g_interpolation_amt;

					for (int k=0; k<g_interpolation_amt; k++) {
						glBegin(GL_LINE_STRIP);
						for (int l=0; l<100; l++) {
							if (abs(g_current_index - i) < smooth_draw_amt) {
								glColor3f((float) (100 - l) * 0.8 / 100, (float) (100 - l) * 0.3 / 100,
										(float) (100 - l) * 0.3 / 100);
							} else {
								glColor3f((float) (100 - l) * 0.6 / 100, (float) (100 - l) * 0.5 / 100,
										(float) (100 - l) * 0.5 / 100);
							}
							float height = top_val_to_the_left + k*y_inc;
							float height_inc = ((float) impulseWindow->margin_bottom - height) / 100;
							glVertex3f(impulseWindow->margin_left + x_values[i-1] + x_inc*k, height + height_inc*l, 0.0f);
						}
						glEnd();
					}

				}

				if (g_current_index == i && mouseIsInDrawingArea() && noElementsAreSelected() && !mouseCloseToALine()) {
					glLineWidth(6);
				} else if (abs(g_current_index - i) < smooth_draw_amt && !mouseCloseToALine() && mouseIsInDrawingArea()) {
					glLineWidth(3);
				} else {
					glLineWidth(1);
				}

				glBegin(GL_LINE_STRIP);
				for (j = 0; j < 100; j++) {
					if (g_current_index == i && mouseIsInDrawingArea() && noElementsAreSelected() && !mouseCloseToALine()) {
						glColor3f((float) (100 - j) * 1.0 / 100, (float) (100 - j) * 0.1 / 100,
								(float) (100 - j) * 0.1 / 100);
					} else if (abs(g_current_index - i) < smooth_draw_amt && !mouseCloseToALine() && mouseIsInDrawingArea()) {
						glColor3f((float) (100 - j) * 0.8 / 100, (float) (100 - j) * 0.3 / 100,
								(float) (100 - j) * 0.3 / 100);
					} else {
						glColor3f((float) (100 - j) * 0.6 / 100, (float) (100 - j) * 0.5 / 100,
								(float) (100 - j) * 0.5 / 100);
					}
					float x_value = index_x_inc*log(i+1)*(float)HALF_FFT_SIZE/log((float)HALF_FFT_SIZE);
					x_values[i] = x_value;
					glVertex3f(impulseWindow->margin_left + x_value, top_value + (float) j * inc, 0.0f);

				}
				glEnd();
				glLineWidth(1);
			}
		}
		glPopMatrix();
	}

	// Draw expected changes
	// If mouse is in middle of window
	if (mouseIsInDrawingArea() && noElementsAreSelected() && !mouseCloseToALine()) {
		int index_left = 0;
		int index_right = HALF_FFT_SIZE - 1;
		if (g_current_index - smooth_draw_amt < 0) {
			index_left = 0;
		} else {
			index_left = g_current_index - smooth_draw_amt + 1;
		}
		if (g_current_index + smooth_draw_amt >= HALF_FFT_SIZE) {
			index_right = HALF_FFT_SIZE - 1;
		} else {
			index_right = g_current_index + smooth_draw_amt - 1;
		}
		int x_value_left = impulseWindow->margin_left + index_x_inc*log(index_left+1)*(float)HALF_FFT_SIZE/log((float)HALF_FFT_SIZE);
		int x_value_current = impulseWindow->margin_left + index_x_inc*log(g_current_index+1)*(float)HALF_FFT_SIZE/log((float)HALF_FFT_SIZE);
		int x_value_right = impulseWindow->margin_left + index_x_inc*log(index_right+1)*(float)HALF_FFT_SIZE/log((float)HALF_FFT_SIZE);
		float y_value_left = 0.0f;
		float y_value_right = 0.0f;
		if (g_current_channel_view == RIGHT) {
			y_value_left = impulseWindow->margin_top - top_vals_right[index_left]*((float) impulseWindow->margin_bottom - impulseWindow->margin_top)/6;
			y_value_right = impulseWindow->margin_top - top_vals_right[index_right]*((float) impulseWindow->margin_bottom - impulseWindow->margin_top)/6;
		} else if (g_current_channel_view == LEFT) {
			y_value_left = impulseWindow->margin_top - top_vals_left[index_left]*((float) impulseWindow->margin_bottom - impulseWindow->margin_top)/6;
			y_value_right = impulseWindow->margin_top - top_vals_left[index_right]*((float) impulseWindow->margin_bottom - impulseWindow->margin_top)/6;
		}

		float y_value_current = TheMouse.y;
		glBegin(GL_LINE_STRIP);
		glColor4f(0.8f,0.3f,0.3f,0.75);
		glVertex3f(x_value_left, y_value_left, 0.0f);
		glVertex3f(x_value_current, y_value_current, 0.0f);
		glVertex3f(x_value_right, y_value_right, 0.0f);
		glEnd();
	}

	// Draw input frequency spectrum
	glBegin(GL_LINE_STRIP);
	glColor4f(1,1,1,0.3);
	bool sensitivity_too_high = false;
	for (i=0; i<HALF_FFT_SIZE; i++) {
		//		for (j=0; j<14; j++) {
		//			last_input_spectrum[j+1][i] = last_input_spectrum[j][i];
		//		}
		last_input_spectrum[14][i] = last_input_spectrum[13][i];
		last_input_spectrum[13][i] = last_input_spectrum[12][i];
		last_input_spectrum[12][i] = last_input_spectrum[11][i];
		last_input_spectrum[11][i] = last_input_spectrum[10][i];
		last_input_spectrum[10][i] = last_input_spectrum[9][i];
		last_input_spectrum[9][i] = last_input_spectrum[8][i];
		last_input_spectrum[8][i] = last_input_spectrum[7][i];
		last_input_spectrum[7][i] = last_input_spectrum[6][i];
		last_input_spectrum[6][i] = last_input_spectrum[5][i];
		last_input_spectrum[5][i] = last_input_spectrum[4][i];
		last_input_spectrum[4][i] = last_input_spectrum[3][i];
		last_input_spectrum[3][i] = last_input_spectrum[2][i];
		last_input_spectrum[2][i] = last_input_spectrum[1][i];
		last_input_spectrum[1][i] = last_input_spectrum[0][i];
		float inputVal = sqrt(pow(input_spectrum[i].Im, 2) + pow(input_spectrum[i].Re, 2));
		last_input_spectrum[0][i] = inputVal;
		float prev_sum = 0.0f;
		for (j=0; j<16; j++) {
			prev_sum += last_input_spectrum[j][i];
		}
		float displayVal = (prev_sum + inputVal)/16;
		if (displayVal < 0.0f) {
			displayVal = 0.0f;
		}
		if (displayVal*(float) g_input_sensitivity > impulseWindow->margin_bottom - impulseWindow->margin_top) {
			sensitivity_too_high = true;
			if (InputSensitivitySlider.x_pos > InputSensitivitySlider.x_min + 1) {
				InputSensitivitySlider.x_pos--;
			}
		}
		glVertex3f(impulseWindow->margin_left + index_x_inc*log(i+1)*(float)HALF_FFT_SIZE/log((float)HALF_FFT_SIZE), impulseWindow->margin_bottom - displayVal*(float) g_input_sensitivity, 0.0f);
	}
	glEnd();

	// Title label
	glColor3f(1,1,1);

	if (g_changingImpulse) {
		Font(GLUT_BITMAP_HELVETICA_18, "Changing Impulse", g_width/2, 30);
	} else {
		Font(GLUT_BITMAP_HELVETICA_18, "Synthesized Convolution Reverb", g_width/2, 30);
	}

	// Show frequency
	if (mouseIsInDrawingArea() && noElementsAreSelected()) {
		// Identify the index of top_vals array corresponding with the x-value of the mouse position

//		float mouse_x_offset = (TheMouse.x - impulseWindow->margin_left) / (float) (impulseWindow->margin_right - impulseWindow->margin_left);

		float width = (float) SAMPLE_RATE / (float) FFT_SIZE;

		int index = 0;

		for (int i=0; i<HALF_FFT_SIZE; i++) {
			if ((TheMouse.x - impulseWindow->margin_left) > x_values[i]) {
				index = i;
				g_current_index = i;
			}
		}

		float frequency = width*(index+1); // index + 1 because bin 0 (DC) is not displayed

		//		printf("Index: %d\n", index);
		if (!mouseCloseToALine()) {
			char buf[20];
			if (frequency < 1000) {
				snprintf(buf, 20, "%f", frequency);
				Font(GLUT_BITMAP_HELVETICA_12, strcat(buf, " Hz"), TheMouse.x, TheMouse.y);
			} else {
				snprintf(buf, 20, "%f", frequency/1000);
				Font(GLUT_BITMAP_HELVETICA_12, strcat(buf, " kHz"), TheMouse.x, TheMouse.y);
			}
		}
	}

	glutSwapBuffers();
	glFlush();
}

/*
 * This function takes an FFT of a portion of audio from the g_input_storage_buffer,
 * zero-pads it to twice its length, multiplies it with a specific block of FFT
 * data from the impulse, takes the IFFT of the resulting data, and places that
 * data in the g_output_storage_buffer after the correct amount of callback cycles.
 */
void *calculateFFT(void *incomingFFTArgs) {

	pthread_detach(pthread_self());

//	int state = g_changes_made;

	FFTArgs *fftArgs = (FFTArgs *) incomingFFTArgs;

	int i;

	int numCyclesToWait = fftArgs->num_callbacks_to_complete - 1;

	int counter_target = (fftArgs->counter + numCyclesToWait)
					% (g_max_factor * 2);
	if (counter_target == 0) {
		counter_target = g_max_factor * 2;
	}

	//	 1. Create buffer with length = 2 * (last_sample_index - first_sample_index),
	//	    fill the buffer with 0s.
	int blockLength = fftArgs->last_sample_index - fftArgs->first_sample_index
			+ 1;
	int convLength = blockLength * 2;

	int volumeFactor = blockLength / g_block_length; // 1, 2, 4, 8, etc

	complex *inputAudio = calloc(convLength, sizeof(complex));
	// 2. Take audio from g_input_storage_buffer (first_sample_index to last_sample_index)
	//    and place it into the buffer created in part 1 (0 to (last_sample_index - first_sample_index)).
	for (i = 0; i < blockLength; i++) {
		inputAudio[i].Re = g_input_storage_buffer[fftArgs->first_sample_index
												  + i];
	}

	//	printf(
	//			"Thread %d: Start convolving sample %d to %d with h%d. This process will take %d cycles and complete when N = %d.\n",
	//			pthread_self(), fftArgs->first_sample_index, fftArgs->last_sample_index,
	//			fftArgs->impulse_block_number, numCyclesToWait + 1, counter_target);

	// 3. Take the FFT of the buffer created in part 1.
	complex *temp = calloc(convLength, sizeof(complex));
	fft(inputAudio, convLength, temp);

	// 4. Determine correct impulse FFT block based in impulse_block_number. The length of this
	//	  block should automatically be the same length as the length of the buffer created in part 1
	//    that now holds the input audio data.
	int fftBlockNumber = fftArgs->impulse_block_number;

	// If the impulse is mono
	if (g_impulse->numChannels == MONO) {

		// 5. Create buffer of length 2 * (last_sample_index - first_sample_index) to hold the result of
		//    FFT multiplication.
		complex *convResult = calloc(convLength, sizeof(complex));
		// 6. Complex multiply the buffer created in part 1 with the impulse FFT block determined in part 4,
		//    and store the result in the buffer created in part 5.
		complex c;

		for (i = 0; i < convLength; i++) {

			//			if (g_changingImpulse || g_changes_made != state) {
			//				free(convResult);
			//				free(temp);
			//				free(inputAudio);
			//				free(fftArgs);
			//				pthread_exit(NULL);
			//				return NULL;
			//			}

			c = complex_mult(inputAudio[i],
					g_fftData_ptr->fftBlocks1[fftBlockNumber][i]);

			convResult[i].Re = c.Re;
			convResult[i].Im = c.Im;

		}

		// 7. Take the IFFT of the buffer created in part 5.
		ifft(convResult, convLength, temp);

		// 8. When the appropriate number of callback cycles have passed (num_callbacks_to_complete), put
		//    the real values of the buffer created in part 5 into the g_output_storage_buffer
		//    (sample 0 through sample 2 * (last_sample_index - first_sample_index)
		while (g_counter != counter_target) {
			if (g_changingImpulse) {
				pthread_exit(NULL);
			}
			nanosleep((const struct timespec[] ) { {0,g_block_duration_in_nanoseconds/NUM_CHECKS_PER_CYCLE}}, NULL);
		}

		pthread_mutex_lock(&mutex);
		// Put data in output buffer
		if (g_output_storage_buffer1_length < convLength) {
			convLength = g_output_storage_buffer1_length;
		}
		for (i = 0; i < convLength; i++) {
			g_output_storage_buffer1[i] += convResult[i].Re / volumeFactor;
		}
		pthread_mutex_unlock(&mutex);

		free(convResult);
	}

	// If the impulse is stereo
	if (g_impulse->numChannels == STEREO) {

		// 5. Create buffer of length 2 * (last_sample_index - first_sample_index) to hold the result of
		//    FFT multiplication.
		complex *convResultLeft = calloc(convLength, sizeof(complex));
		complex *convResultRight = calloc(convLength, sizeof(complex));

		// 6. Complex multiply the buffer created in part 1 with the impulse FFT block determined in part 4,
		//    and store the result in the buffer created in part 5.
		complex c1, c2;

		for (i = 0; i < convLength; i++) {
			// Left channel
			c1 = complex_mult(inputAudio[i],
					g_fftData_ptr->fftBlocks1[fftBlockNumber][i]);
			// Right channel
			c2 = complex_mult(inputAudio[i],
					g_fftData_ptr->fftBlocks2[fftBlockNumber][i]);

			convResultLeft[i].Re = c1.Re;
			convResultLeft[i].Im = c1.Im;

			convResultRight[i].Re = c2.Re;
			convResultRight[i].Im = c2.Im;

		}

		// 7. Take the IFFT of the buffer created in part 5.
		ifft(convResultLeft, convLength, temp);
		ifft(convResultRight, convLength, temp);

		// 8. When the appropriate number of callback cycles have passed (num_callbacks_to_complete), put
		//    the real values of the buffer created in part 5 into the g_output_storage_buffer
		//    (sample 0 through sample 2 * (last_sample_index - first_sample_index)
		while (g_counter != counter_target) {
			if (g_changingImpulse) {
				pthread_exit(NULL);
			}
			nanosleep((const struct timespec[] ) { {0,g_block_duration_in_nanoseconds/NUM_CHECKS_PER_CYCLE}}, NULL);
		}

		pthread_mutex_lock(&mutex);
		// Put data in output buffer
		if (g_output_storage_buffer1_length < convLength) {
			convLength = g_output_storage_buffer1_length;
		}
		for (i = 0; i < convLength; i++) {
			g_output_storage_buffer1[i] += convResultLeft[i].Re / volumeFactor; // left channel
			g_output_storage_buffer2[i] += convResultRight[i].Re / volumeFactor; // right channel
		}
		pthread_mutex_unlock(&mutex);

		free(convResultLeft);
		free(convResultRight);

	}

	//	printf(
	//			"Thread %d: The result of the convolution of sample %d to %d with h%d has been added to the output buffer. Expected arrival: when n = %d.\n",
	//			pthread_self(), fftArgs->first_sample_index, fftArgs->last_sample_index,
	//			fftArgs->impulse_block_number, counter_target);

	// Free remaining buffers
	free(temp);
	free(inputAudio);

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
	paData *data = (paData *) userData;
	if (AUDIO_FILE_INPUT) {
		int readcount;
		readcount = sf_readf_float(data->infile1, data->buffer1, framesPerBuffer);

		if (readcount < framesPerBuffer) {
			sf_seek(data->infile1, 0, SEEK_SET);
			readcount = sf_readf_float(data->infile1, data->buffer1+(readcount*data->channels), framesPerBuffer-readcount);
		}
	}

	float mult_factor = 0.00001f;

	float *inBuf = (float*) inputBuffer;
	float *outBuf = (float*) outputBuffer;

	int i, j;

	if (!g_changingImpulse) {

		// If the impulse is mono
		if (g_impulse->numChannels == MONO) {

			float total = 0.0f;

			for (i = 0; i < framesPerBuffer; i++) {
				total += fabsf(g_output_storage_buffer1[i]*mult_factor);
			}

			total /= (float) framesPerBuffer;

			if (total > g_loudest) {
				g_loudest = total;
				//				printf("New loudest value: %f\n", g_loudest);
			}

			//			printf("Avg value: %f\n", total);

			if (total > 0.5f) {
				for (i = 0; i < framesPerBuffer; i++) {
					outBuf[i] = 0.0f;
				}
				g_consecutive_skipped_cycles++;
				printf("Output was too loud (%f) and was automatically muted.\n", total);
				if (g_consecutive_skipped_cycles > SAMPLE_RATE/(2*framesPerBuffer)) {
					RecomputeImpulseButtonCallback();
					g_consecutive_skipped_cycles = 0;
				}
			} else {
				g_consecutive_skipped_cycles = 0;
				pthread_mutex_lock(&mutex);
				for (i = 0; i < framesPerBuffer; i++) {
					outBuf[i] = ((float) g_dry_wet/100)*g_output_storage_buffer1[i]*mult_factor + ((float) (100-g_dry_wet)/100)*g_input_storage_buffer[g_input_storage_buffer_length
																																					   - g_block_length + i];
				}
				pthread_mutex_unlock(&mutex);
			}
		}

		// If the impulse is stereo
		if (g_impulse->numChannels == STEREO) {

			float total_left = 0.0f;
			float total_right = 0.0f;

			for (i = 0; i < framesPerBuffer; i++) {
				total_left += fabsf(g_output_storage_buffer1[i]*mult_factor);
				total_right += fabsf(g_output_storage_buffer2[i]*mult_factor);
			}

			total_left /= (float) (framesPerBuffer);
			total_right /= (float) (framesPerBuffer);

			if (total_left > g_loudest) {
				g_loudest = total_left;
				//				printf("New loudest value: %f\n", g_loudest);
			}
			if (total_right > g_loudest) {
				g_loudest = total_right;
				//				printf("New loudest value: %f\n", g_loudest);
			}

			//			printf("Avg value: %f\n", (total_left + total_right / 2));

			if (total_left > 0.5f) {
				for (i = 0; i < framesPerBuffer * 2; i++) {
					outBuf[i] = 0.0f;
				}
				g_consecutive_skipped_cycles++;
				printf("Output was too loud (%f) and was automatically muted.\n", total_left);
				if (g_consecutive_skipped_cycles > SAMPLE_RATE/(2*framesPerBuffer)) {
					RecomputeImpulseButtonCallback();
					g_consecutive_skipped_cycles = 0;
				}
			} else if (total_right > 0.5f) {
				for (i = 0; i < framesPerBuffer * 2; i++) {
					outBuf[i] = 0.0f;
				}
				g_consecutive_skipped_cycles++;
				printf("Output was too loud (%f) and was automatically muted.\n", total_right);
				if (g_consecutive_skipped_cycles > SAMPLE_RATE/(2*framesPerBuffer)) {
					RecomputeImpulseButtonCallback();
					g_consecutive_skipped_cycles = 0;
				}
			} else {
				g_consecutive_skipped_cycles = 0;
				pthread_mutex_lock(&mutex);
				for (i = 0; i < framesPerBuffer; i++) {
					outBuf[2 * i] = ((float) g_dry_wet/100)*g_output_storage_buffer1[i]*mult_factor + ((float) (100-g_dry_wet)/100)*g_input_storage_buffer[g_input_storage_buffer_length
																																						   - g_block_length + i];
					outBuf[2 * i + 1] = ((float) g_dry_wet/100)*g_output_storage_buffer2[i]*mult_factor + ((float) (100-g_dry_wet)/100)*g_input_storage_buffer[g_input_storage_buffer_length
																																							   - g_block_length + i];
				}
				pthread_mutex_unlock(&mutex);
			}
		}

		++g_counter;

		if (g_counter >= g_max_factor * 2 + 1) {
			g_counter = 1;
		}

		// Shift g_input_storage_buffer to the left by g_block_length
		for (i = 0; i < g_input_storage_buffer_length - g_block_length; i++) {
			g_input_storage_buffer[i] = g_input_storage_buffer[i
															   + g_block_length];
		}

		if (AUDIO_FILE_INPUT) {
			if (data->channels == MONO) {
				for (i = 0; i < g_block_length; i++) {
					g_input_storage_buffer[g_input_storage_buffer_length
										   - g_block_length + i] = data->buffer1[i];
				}
			}
			if (data->channels == STEREO) {
				for (i = 0; i < g_block_length; i++) {
					g_input_storage_buffer[g_input_storage_buffer_length
										   - g_block_length + i] = (data->buffer1[2*i] + data->buffer1[2*i + 1])/2;
				}
			}
		} else if (LIVE_AUDIO_INPUT) {
			// Fill right-most portion of g_input_storage_buffer with most recent audio
			for (i = 0; i < g_block_length; i++) {
				g_input_storage_buffer[g_input_storage_buffer_length
									   - g_block_length + i] = inBuf[i];
			}
		}

		//
		//		float total_input = 0.0f;
		//
		//		for (i=0; i<g_block_length; i++) {
		//			total_input += g_input_storage_buffer[g_input_storage_buffer_length
		//													- g_block_length + i];
		//		}
		//
		//		printf("Total input: %f\n", total_input);

		/*
		 * Create threads
		 */
		for (j = 0; j < g_powerOf2Vector.size; j++) {
			int factor = vector_get(&g_powerOf2Vector, j);
			if (g_counter % factor == 0 && g_counter != 0) {

				/*
				 * Take the specified samples from the input_storage_buffer, zero-pad them to twice their
				 * length, FFT them, multiply the resulting spectrum by the corresponding impulse FFT block,
				 * IFFT the result, put the result in the output_storage_buffer.
				 */
				FFTArgs *fftArgs = (FFTArgs *) malloc(sizeof(FFTArgs));

				fftArgs->first_sample_index = (1 + g_end_sample
						- g_block_length * factor);
				fftArgs->last_sample_index = g_end_sample;
				fftArgs->impulse_block_number = (j * 2 + 1);
				fftArgs->num_callbacks_to_complete = factor;
				fftArgs->counter = g_counter;
				pthread_create(&thread, NULL, calculateFFT, (void *) fftArgs);

				FFTArgs *fftArgs2 = (FFTArgs *) malloc(sizeof(FFTArgs));
				fftArgs2->first_sample_index = (1 + g_end_sample
						- g_block_length * factor);
				fftArgs2->last_sample_index = g_end_sample;
				fftArgs2->impulse_block_number = (j * 2 + 2);
				fftArgs2->num_callbacks_to_complete = factor * 2;
				fftArgs2->counter = g_counter;

				pthread_create(&thread, NULL, calculateFFT, (void *) fftArgs2);

			}
		}

		// Shift g_output_storage_buffer
		pthread_mutex_lock(&mutex);
		for (i = 0; i < g_output_storage_buffer1_length - g_block_length; i++) {
			g_output_storage_buffer1[i] = g_output_storage_buffer1[i
																   + g_block_length];
			g_output_storage_buffer2[i] = g_output_storage_buffer2[i
																   + g_block_length];
		}
		pthread_mutex_unlock(&mutex);

	} else {
		/*
		 * If the impulse is being changed, send zeros to the output rather than
		 * hearing a glitch in the audio
		 */
		if (g_impulse->numChannels == MONO) {
			for (i = 0; i < framesPerBuffer; i++) {
				outBuf[i] = 0.0f;
			}
		} else if (g_impulse->numChannels == STEREO) {
			for (i = 0; i < framesPerBuffer*2; i++) {
				outBuf[i] = 0.0f;
			}
		}

		for (i=0; i<g_output_storage_buffer1_length; i++) {
			g_output_storage_buffer1[i] = 0.0f;
			g_output_storage_buffer2[i] = 0.0f;
		}
		for (i=0; i<g_input_storage_buffer_length; i++) {
			g_input_storage_buffer[i] = 0.0f;
		}
	}

	complex *input_temp = (complex *) malloc(MIN_FFT_BLOCK_SIZE * sizeof(complex));

	// This assumes mono input
	if (LIVE_AUDIO_INPUT) {
		for (i=0; i<MIN_FFT_BLOCK_SIZE; i++) {
			input_spectrum[i].Im = 0.0f;
			input_spectrum[i].Re = inBuf[i];
		}
	}
	if (AUDIO_FILE_INPUT) {
		for (i=0; i<MIN_FFT_BLOCK_SIZE; i++) {
			input_spectrum[i].Im = 0.0f;
			input_spectrum[i].Re = data->buffer1[i];
		}
	}


	fft(input_spectrum,MIN_FFT_BLOCK_SIZE,input_temp);
	//
	//	for (i=0; i<MIN_FFT_BLOCK_SIZE; i++) {
	//		printf("Magnitude[%d]: %f\n", i, sqrt(pow(input_spectrum[i].Im, 2) + pow(input_spectrum[i].Re, 2)));
	//	}

	free(input_temp);

	return paContinue;
}

/*
 * This function is responsible for starting PortAudio and OpenGL.
 */
void runPortAudio() {

	if (AUDIO_FILE_INPUT) {
		paData data;
		PaStream* stream;
		PaStreamParameters outputParams;
		PaError err;
		memset(&data.sfinfo1, 0, sizeof(data.sfinfo1));
		data.infile1 = sf_open(audio_file_name, SFM_READ, &data.sfinfo1);
		if (data.infile1 == NULL) {
			printf("Error: could not open file: %s\n", audio_file_name);
			puts(sf_strerror(NULL));
			exit(1);
		}
		data.sampleRate = data.sfinfo1.samplerate;
		data.amplitude1 = 1.0f;
		data.channels = data.sfinfo1.channels;

		err = Pa_Initialize();
		if (err != paNoError ) {
			printf("PortAudio error: %s\n", Pa_GetErrorText(err));
			printf("\nExiting.\n");
			exit(1);
		}

		/* Ouput stream parameters */
		outputParams.device = Pa_GetDefaultOutputDevice();
		outputParams.channelCount = g_impulse->numChannels;
		outputParams.sampleFormat = paFloat32;
		outputParams.suggestedLatency =
				Pa_GetDeviceInfo(outputParams.device)->defaultLowOutputLatency;
		outputParams.hostApiSpecificStreamInfo = NULL;

		/* Open audio stream */
		err = Pa_OpenStream(&stream,
				NULL, /* no input */
				&outputParams,
				data.sampleRate,
				MIN_FFT_BLOCK_SIZE,
				paNoFlag, /* flags */
				paCallback,
				&data);

		if (err != paNoError) {
			printf("PortAudio error: open stream: %s\n", Pa_GetErrorText(err));
			exit(2);
		}
		err = Pa_StartStream(stream);
		if (err != paNoError) {
			printf(  "PortAudio error: start stream: %s\n", Pa_GetErrorText(err));
			exit(3);
		}

		glutMainLoop();

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

		sf_close(data.infile1);

	} else {
		PaStream* stream;
		PaStreamParameters outputParameters;
		PaStreamParameters inputParameters;
		PaError err;
		/* Initialize PortAudio */
		Pa_Initialize();
		/* Set output stream parameters */
		outputParameters.device = Pa_GetDefaultOutputDevice();
		outputParameters.channelCount = g_impulse->numChannels;
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

		if (err != paNoError) {
			printf("PortAudio error: open stream: %s\n", Pa_GetErrorText(err));
		}
		/* Start audio stream */
		err = Pa_StartStream(stream);
		if (err != paNoError) {
			printf("PortAudio error: start stream: %s\n", Pa_GetErrorText(err));
		}

		glutMainLoop();

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
}

/*
 * This function takes an impulse and returns its frequency data over time
 * in a 2-dimensional buffer.
 *
 * float **impulse_filter_env_blocks[i][j], where i = time (in blocks)
 * and j = frequency bin number
 *
 */
float **getImpulseFFTBlocks(audioData *impulse_from_file, int channel) {

	int i, j;
	/*
	 * Get FFT profile for impulse
	 */
	int num_impulse_blocks = (impulse_from_file->numFrames / FFT_SIZE);

	// Allocate memory for array of filter envelope blocks
	float **impulse_filter_env_blocks = (float **) malloc(
			sizeof(float *) * num_impulse_blocks);

	// Allocate memory each individual filter envelope
	for (i = 0; i < num_impulse_blocks; i++) {
		impulse_filter_env_blocks[i] = (float *) malloc(
				sizeof(float) * FFT_SIZE);
	}

	for (i = 0; i < num_impulse_blocks; i++) {

		// Allocate memory for FFT
		complex *fftBlock = (complex *) calloc(FFT_SIZE, sizeof(complex));
		complex *temp = (complex *) calloc(FFT_SIZE, sizeof(complex));

		if (channel == LEFT) {
			// Copy impulse into fft buffer
			for (j = 0; j < FFT_SIZE; j++) {
				fftBlock[j].Re = impulse_from_file->buffer1[i * FFT_SIZE + j];
			}
		} else if (channel == RIGHT) {
			// Copy impulse into fft buffer
			for (j = 0; j < FFT_SIZE; j++) {
				fftBlock[j].Re = impulse_from_file->buffer2[i * FFT_SIZE + j];
			}
		}

		// Take FFT of block
		fft(fftBlock, FFT_SIZE, temp);

		/*
		 * Actually apply frequency-domain filter
		 */
		for (j = 0; j < FFT_SIZE; j++) {

			/*
			 * Obtain magnitude for each frequency bin
			 */
			impulse_filter_env_blocks[i][j] = sqrt(
					pow(fftBlock[j].Re, 2) + pow(fftBlock[j].Im, 2));

		}

		free(temp);
		free(fftBlock);

	}
	return impulse_filter_env_blocks;
}

//-----------------------------------------------------------------------------
// name: hanning()
// desc: make window
//-----------------------------------------------------------------------------
void create_hanning(float * window, unsigned long length) {
	unsigned long i;
	double pi, phase = 0, delta;

	pi = 4. * atan(1.0);
	delta = 2 * pi / (double) length;

	for (i = 0; i < length; i++) {
		window[i] = (float) (0.5 * (1.0 - cos(phase)));
		phase += delta;
	}
}

float **getExponentialFitFromGraph(int num_impulse_blocks, int channel) {

	int i, j;

	float **impulse_filter_env_blocks_exp_fit = (float **) malloc(
			sizeof(float *) * HALF_FFT_SIZE);

	for (i = 0; i < HALF_FFT_SIZE; i++) {
		impulse_filter_env_blocks_exp_fit[i] = (float *) calloc(
				num_impulse_blocks, sizeof(float));
	}

	float *x = (float *) malloc(sizeof(float) * num_impulse_blocks);

	for (i = 0; i < num_impulse_blocks; i++) {
		x[i] = i;
	}

	//	printf("x2: %d\n", (num_impulse_blocks-1));

	if (channel == LEFT) {
		for (i = 0; i < HALF_FFT_SIZE; i++) {
			float y1 = (top_vals_left[i] + (g_height_top - g_height_bottom)) * g_max
					/ (g_height_top - g_height_bottom);
			float y2 = bottom_vals_left[i];
			//		float x1 = 0.0f;
			float x2 = num_impulse_blocks - 1;

			//		float sum_x = x2 + x1;
			//		float sum_temp = y1 + y2;
			//		float sum_x_times_x = pow(x1, 2) + pow(x2, 2);
			//		float sum_temp_times_x = x1 * y1 + x2 * y2;
			//
			//		float b = (2 * sum_temp_times_x - sum_x * sum_temp)
			//				/ (2 * sum_x_times_x - sum_x * sum_x);
			//		float a = (sum_temp - b * sum_x) / 2;
			//
			//		float A = exp(a);
			//
			float a = y1;

			float b = pow((y2 / a), (1 / x2));

			//		printf("y1: %f, y2: %f, b: %f\n", y1, y2, b);

			//		printf(
			//				"top_vals_left[i]: %f, y1: %f, y2: %f, x1: %f, x2: %f, b: %f, a: %f, A: %f\n",
			//				top_vals_left[i], y1, y2, x1, x2, b, a, A);

			for (j = 0; j < num_impulse_blocks; j++) {
				impulse_filter_env_blocks_exp_fit[i][j] = a * pow(b, x[j]);
				//			if (i == 0) {
				//				printf("impulse_filter_env_blocks_exp_fit[%d][%d]: %f\n", i, j,
				//						impulse_filter_env_blocks_exp_fit[i][j]);
				//			}
			}

		}
	} else if (channel == RIGHT) {
		for (i = 0; i < HALF_FFT_SIZE; i++) {
			float y1 = (top_vals_right[i] + (g_height_top - g_height_bottom)) * g_max
					/ (g_height_top - g_height_bottom);
			float y2 = bottom_vals_right[i];
			//		float x1 = 0.0f;
			float x2 = num_impulse_blocks - 1;

			//		float sum_x = x2 + x1;
			//		float sum_temp = y1 + y2;
			//		float sum_x_times_x = pow(x1, 2) + pow(x2, 2);
			//		float sum_temp_times_x = x1 * y1 + x2 * y2;
			//
			//		float b = (2 * sum_temp_times_x - sum_x * sum_temp)
			//				/ (2 * sum_x_times_x - sum_x * sum_x);
			//		float a = (sum_temp - b * sum_x) / 2;
			//
			//		float A = exp(a);
			//
			float a = y1;

			float b = pow((y2 / a), (1 / x2));

			//		printf("y1: %f, y2: %f, b: %f\n", y1, y2, b);

			//		printf(
			//				"top_vals_left[i]: %f, y1: %f, y2: %f, x1: %f, x2: %f, b: %f, a: %f, A: %f\n",
			//				top_vals_left[i], y1, y2, x1, x2, b, a, A);

			for (j = 0; j < num_impulse_blocks; j++) {
				impulse_filter_env_blocks_exp_fit[i][j] = a * pow(b, x[j]);
				//			if (i == 0) {
				//				printf("impulse_filter_env_blocks_exp_fit[%d][%d]: %f\n", i, j,
				//						impulse_filter_env_blocks_exp_fit[i][j]);
				//			}
			}

		}
	}


	return impulse_filter_env_blocks_exp_fit;
}

/*
 * This function takes the frequency data of an impulse, computes best-fit exponential
 * functions for each frequency bin over time, and returns the values calculated from
 * these exponential functions in a 2-dimensional buffer.
 *
 * float **impulse_filter_env_blocks_exp_fit[i][j], where i = frequency bin number
 * and j = time (in blocks).
 */
float **getExponentialFitForImpulseFFTBlocks(int num_impulse_blocks,
		int length_before_zero_padding, float **impulse_filter_env_blocks) {

	int i, j;

	int num_nonzero_impulse_blocks = ceil(
			(float) length_before_zero_padding / FFT_SIZE);

	/*
	 * For each impulse_filter_env block, create exponential fit (to smooth out filter decay)
	 */
	float **impulse_filter_env_blocks_exp_fit = (float **) malloc(
			sizeof(float *) * FFT_SIZE / 2);
	for (i = 0; i < FFT_SIZE / 2; i++) {
		impulse_filter_env_blocks_exp_fit[i] = (float *) calloc(
				num_impulse_blocks, sizeof(float));
	}

	int n = num_nonzero_impulse_blocks;
	float *x = (float *) malloc(sizeof(float) * num_impulse_blocks);
	for (i = 0; i < num_impulse_blocks; i++) {
		x[i] = i;
	}

	for (i = 0; i < FFT_SIZE / 2; i++) {
		float *temp = (float *) malloc(
				sizeof(float) * num_nonzero_impulse_blocks);
		for (j = 0; j < n; j++) {
			temp[j] = log(impulse_filter_env_blocks[j][i + 1]); // i + 1 because index of 0 = DC
		}

		float sum_x_times_x = 0.0f;
		float sum_temp_times_x = 0.0f;
		float sum_x = 0.0f;
		float sum_temp = 0.0f;

		for (j = 0; j < n; j++) {
			sum_x += x[j];
			sum_temp += temp[j];
			sum_x_times_x += pow(x[j], 2);
			sum_temp_times_x += temp[j] * x[j];
		}

		float b = (n * sum_temp_times_x - sum_x * sum_temp)
						/ (n * sum_x_times_x - sum_x * sum_x);
		float a = (sum_temp - b * sum_x) / n;

		float A = exp(a);
		for (j = 0; j < num_impulse_blocks; j++) {

			impulse_filter_env_blocks_exp_fit[i][j] = A * exp(b * x[j]);
			//			if (i == 0) {
			//				printf("impulse_filter_env_blocks_exp_fit[%d][%d]: %f\n", i, j,
			//						impulse_filter_env_blocks_exp_fit[i][j]);
			//			}
		}

		if (g_max < impulse_filter_env_blocks_exp_fit[i][0]) {
			g_max = impulse_filter_env_blocks_exp_fit[i][0];
		}

	}

	return impulse_filter_env_blocks_exp_fit;
}

/*
 * This function computes the amplitude envelope of an impulse.
 *
 * float *envelope[i], where i = sample number
 */
float *getAmplitudeEnvelope(audioData *impulse_from_file, int channel) {
	int amp_envelope_length = impulse_from_file->numFrames / SMOOTHING_AMT;

	float *avg_amplitudes = (float *) malloc(
			sizeof(float) * amp_envelope_length);

	int i, j;
	for (i = 0; i < amp_envelope_length; i++) {

		float sum = 0;

		if (channel == LEFT) {
			for (j = 0; j < SMOOTHING_AMT; j++) {
				sum += fabsf(impulse_from_file->buffer1[i * SMOOTHING_AMT + j]);
			}
		} else if (channel == RIGHT) {
			for (j = 0; j < SMOOTHING_AMT; j++) {
				sum += fabsf(impulse_from_file->buffer2[i * SMOOTHING_AMT + j]);
			}
		}



		sum /= SMOOTHING_AMT;

		avg_amplitudes[i] = sum;

	}

	/*
	 * This envelope buffer holds an amplitude envelope that mirrors the
	 * shape of the original impulse
	 */
	float *envelope = (float *) malloc(
			sizeof(float) * impulse_from_file->numFrames);

	for (i = 0; i < (amp_envelope_length - 1); i++) {

		float difference = avg_amplitudes[i + 1] - avg_amplitudes[i];

		float increment = difference / (float) SMOOTHING_AMT;

		for (j = 0; j < SMOOTHING_AMT; j++) {
			envelope[i * SMOOTHING_AMT + j] = avg_amplitudes[i]
															 + (j * increment);
		}

	}

	for (i = 0; i < impulse_from_file->numFrames; i++) {
		if (envelope[i] < 0.000001) {
			envelope[i] = 0.000001;
		}
	}
	return envelope;
}

/*
 * This function computes an exponential fit for an amplitude envelope.
 */
float *getExponentialFitForAmplitudeEnvelope(float *envelope,
		audioData *impulse_from_file) {

	int length = impulse_from_file->numFrames;

	float *exp_fit = (float *) malloc(sizeof(float) * length);

	float *temp = (float *) malloc(sizeof(float) * length);

	int i;

	float *x = (float *) malloc(sizeof(float) * length);
	for (i = 0; i < length; i++) {
		x[i] = i;
	}

	for (i = 0; i < length; i++) {
		temp[i] = log(envelope[i]);
	}

	float sum_x_times_x = 0.0f;
	float sum_temp_times_x = 0.0f;
	float sum_x = 0.0f;
	float sum_temp = 0.0f;

	for (i = 0; i < length; i++) {
		sum_x += x[i];
		sum_temp += temp[i];
		sum_x_times_x += pow(x[i], 2);
		sum_temp_times_x += temp[i] * x[i];
	}

	float b = (length * sum_temp_times_x - sum_x * sum_temp)
					/ (length * sum_x_times_x - sum_x * sum_x);
	float a = (sum_temp - b * sum_x) / length;

	float A = exp(a);

	for (i = 0; i < length; i++) {
		exp_fit[i] = A * exp(b * x[i]);
	}

	return exp_fit;
}
/*
 * This function filters white noise using the exponential fit filter data.
 *
 * float *synthesized_impulse_buffer[i], where i = sample number.
 */
float *getFilteredWhiteNoise(audioData *impulse_from_file,
		float **impulse_filter_env_blocks_exp_fit) {
	int i, j;

	// Buffer to hold processed audio
	float *synthesized_impulse_buffer = (float *) calloc(
			impulse_from_file->numFrames, sizeof(float));

	int numBlocks = (impulse_from_file->numFrames / FFT_SIZE);

	for (i = 0; i < numBlocks; i++) {

		// Allocate memory for FFT
		complex *fftBlock = (complex *) calloc(FFT_SIZE * 2, sizeof(complex));
		complex *temp = (complex *) calloc(FFT_SIZE * 2, sizeof(complex));

		// Put white noise into fft buffer
		for (j = 0; j < FFT_SIZE * 2; j++) {

			fftBlock[j].Re = ((float) rand() / RAND_MAX) * 2.0f - 1.0f;
		}

		// Take FFT of block
		fft(fftBlock, FFT_SIZE * 2, temp);

		/*
		 * Actually apply frequency-domain filter
		 */

//		// set DC to 0
//		fftBlock[0].Re = 0.1f;
//		fftBlock[0].Im = 0.0f;
//
//		// If FFT_SIZE = 512, then this goes from 1 to 256 inclusive
//		for (j = 1; j < FFT_SIZE / 2 + 1; j++) {
//			fftBlock[2*j - 1].Re *= impulse_filter_env_blocks_exp_fit[j-1][i];
//			fftBlock[2*j].Re *= impulse_filter_env_blocks_exp_fit[j-1][i];
//			fftBlock[2 * FFT_SIZE - 2*j + 1].Re *= impulse_filter_env_blocks_exp_fit[j-1][i];
//			fftBlock[2 * FFT_SIZE - 2*j].Re *= impulse_filter_env_blocks_exp_fit[j-1][i];
//		}


		for (j = 0; j < FFT_SIZE; j++) {
			if (j < FFT_SIZE / 2) {
				fftBlock[2 * j].Re *= impulse_filter_env_blocks_exp_fit[j][i];
				fftBlock[2 * j + 1].Re *=
						impulse_filter_env_blocks_exp_fit[j][i];
				fftBlock[2 * j].Im *= impulse_filter_env_blocks_exp_fit[j][i];
				fftBlock[2 * j + 1].Im *=
						impulse_filter_env_blocks_exp_fit[j][i];
			} else {
				fftBlock[2 * j].Re *= impulse_filter_env_blocks_exp_fit[FFT_SIZE
																		- j - 1][i];
				fftBlock[2 * j + 1].Re *=
						impulse_filter_env_blocks_exp_fit[FFT_SIZE - j - 1][i];
				fftBlock[2 * j].Im *= impulse_filter_env_blocks_exp_fit[FFT_SIZE
																		- j - 1][i];
				fftBlock[2 * j + 1].Im *=
						impulse_filter_env_blocks_exp_fit[FFT_SIZE - j - 1][i];
			}
		}



		ifft(fftBlock, FFT_SIZE * 2, temp);

		float *fftBlock_float = (float *) malloc(sizeof(float) * FFT_SIZE * 2);

		for (j = 0; j < FFT_SIZE * 2; j++) {

			fftBlock_float[j] = fftBlock[j].Re;

		}

		float *window = (float *) malloc(sizeof(float) * FFT_SIZE * 2);
		hanning(window, FFT_SIZE * 2);

		for (j = 0; j < FFT_SIZE * 2; j++) {

			if (i * FFT_SIZE + j < impulse_from_file->numFrames) {
				synthesized_impulse_buffer[i * FFT_SIZE + j] +=
						fftBlock_float[j] * window[j];
			}

		}

		free(temp);
		free(fftBlock_float);
		free(fftBlock);
		free(window);

	}
	return synthesized_impulse_buffer;
}

/*
 * This function actually applies the amplitude envelope to the filtered white
 * noise buffer.
 */
void applyAmplitudeEnvelope(audioData *impulse_from_file,
		float *synthesized_impulse_buffer, float *envelope) {
	float output_max = 0.0f;
	int i;
	float *exp_fit = getExponentialFitForAmplitudeEnvelope(envelope,
			impulse_from_file);

	float *differences = (float *) malloc(
			sizeof(float) * impulse_from_file->numFrames);

	for (i = 0; i < impulse_from_file->numFrames; i++) {
		differences[i] = envelope[i] / exp_fit[i];
		//		printf("differences[%d]: %f/%f: %f\n", i, envelope[i], exp_fit[i], differences[i]);
	}

	/*
	 * Apply amplitude envelope
	 */
	for (i = 0; i < impulse_from_file->numFrames; i++) {

		//		synthesized_impulse_buffer[i] *= differences[i];

		if (fabsf(synthesized_impulse_buffer[i]) > output_max) {
			output_max = fabsf(synthesized_impulse_buffer[i]);
		}

	}

//	output_max /= synthesized_impulse_gain_factor;

	for (i = 0; i < impulse_from_file->numFrames; i++) {
		synthesized_impulse_buffer[i] /= output_max;
	}
}

/*
 * This function stores initial exponential fit values (for the first block of data)
 * so that it can be visually displayed using OpenGL.
 */
void setTopValsBasedOnImpulseFFTBlocks(
		float **impulse_filter_env_blocks_exp_fit, int channel) {
	int i;

	if (channel == LEFT) {
		for (i = 0; i < FFT_SIZE / 2; i++) {

			// Divide by g_max in order to normalize values
			// g_max = maximum complex amplitude of impulse
			top_vals_left[i] = ((g_height_top - g_height_bottom) - (impulse_filter_env_blocks_exp_fit[i][0] * (g_height_top - g_height_bottom) / g_max)) * -1;

			//		printf("exp_fit_val[%d][0]: %f, top_vals_left[%d]: %f\n", i, impulse_filter_env_blocks_exp_fit[i][0], i, top_vals_left[i]);

			bottom_vals_left[i] = 0.0001f;

		}
	} else if (channel == RIGHT) {
		for (i = 0; i < FFT_SIZE / 2; i++) {

			// Divide by g_max in order to normalize values
			top_vals_right[i] = ((g_height_top - g_height_bottom) - (impulse_filter_env_blocks_exp_fit[i][0] * (g_height_top - g_height_bottom) / g_max)) * -1;

			//		printf("exp_fit_val[%d][0]: %f, top_vals_left[%d]: %f\n", i, impulse_filter_env_blocks_exp_fit[i][0], i, top_vals_left[i]);

			bottom_vals_right[i] = 0.0001f;

		}
	}


}

void crossfadeRecordedAndSynthesizedImpulses(float* synthesized_impulse_buffer,
		audioData* impulse_from_file, int channel) {

	int i;

	if (!use_attack_from_impulse) {
		return;
	}

	// Create window for use in crossfading
	float *window = (float *) malloc(sizeof(float) * crossover_length * 2);
	create_hanning(window, crossover_length * 2);

	if (channel == LEFT) {
		// Use original impulse up until crossover point
		for (i = 0; i < crossover_point; i++) {
			synthesized_impulse_buffer[i] = impulse_from_file->buffer1[i];
		}
	} else if (channel == RIGHT) {
		// Use original impulse up until crossover point
		for (i = 0; i < crossover_point; i++) {
			synthesized_impulse_buffer[i] = impulse_from_file->buffer2[i];
		}
	}


	// Fade between original and synthesized impulse over crossover length
	for (i = 0; i < crossover_length; i++) {

		float original_component = 0.0f;
		if (channel == LEFT) {
			// Use second half of hanning window to fade out original component
			original_component = impulse_from_file->buffer1[crossover_point
															+ i] * window[crossover_length + i];
		} else if (channel == RIGHT) {
			// Use second half of hanning window to fade out original component
			original_component = impulse_from_file->buffer2[crossover_point
															+ i] * window[crossover_length + i];
		}

		// Use first half of hanning window to fade in synthesized component
		float synthesized_component = synthesized_impulse_buffer[crossover_point
																 + i] * window[i];

		// Sum the two together to complete the crossfade
		synthesized_impulse_buffer[crossover_point + i] = original_component
				+ synthesized_component;
	}
}

/*
 * This function resynthesizes the impulse whenever a change is made.
 */
audioData *resynthesizeImpulse(audioData *currentImpulse, int newLengthInFrames) {

	int i;
	// Preliminary calculations/processes
	audioData *synth_impulse = (audioData *) malloc(sizeof(audioData));
	//	int length = currentImpulse->numFrames;
	//	int num_impulse_blocks = (currentImpulse->numFrames / FFT_SIZE);

	// Put synthesized impulse in audioData struct
	synth_impulse->numChannels = currentImpulse->numChannels;
	synth_impulse->numFrames = newLengthInFrames;
	synth_impulse->sampleRate = SAMPLE_RATE;
	synth_impulse->buffer1 = (float *) malloc(sizeof(float) * newLengthInFrames * currentImpulse->numChannels);
	synth_impulse->buffer2 = (float *) malloc(sizeof(float) * newLengthInFrames * currentImpulse->numChannels);

	/*
	 * If the impulse is MONO
	 */
	if (synth_impulse->numChannels == MONO) {

		//TODO: Create new exponential fit data based on top_vals and bottom_vals, not on impulse data.
		float **exp_fit = getExponentialFitFromGraph(
				synth_impulse->numFrames / FFT_SIZE, LEFT);

//		setTopValsBasedOnImpulseFFTBlocks(exp_fit, LEFT);

		//Then, filter white noise with this exponential fit data.
		float *synthesized_impulse_buffer = getFilteredWhiteNoise(
				synth_impulse, exp_fit);

		g_amp_envelope = getAmplitudeEnvelope(synth_impulse, LEFT);

		//Then, apply amp envelope.
		applyAmplitudeEnvelope(synth_impulse, synthesized_impulse_buffer,
				g_amp_envelope);

		// crossfade between recorded impulse attack and synthesized tail
		crossfadeRecordedAndSynthesizedImpulses(synthesized_impulse_buffer,
				currentImpulse, LEFT);

		// Write to a wav file
		//		writeWavFile(synthesized_impulse_buffer, SAMPLE_RATE,
		//				synth_impulse->numChannels, synth_impulse->numFrames, 1,
		//				"11_10_2015_test.wav");

		// Put synthesized impulse in audioData struct
		synth_impulse->buffer1 = synthesized_impulse_buffer;

		float max = 0.0f;
		for (i = 0; i < synth_impulse->numFrames; i++) {
			if (fabsf(synth_impulse->buffer1[i]) > max) {
				max = fabsf(synth_impulse->buffer1[i]);
			}
		}

		for (i = 0; i < synth_impulse->numFrames; i++) {
			synth_impulse->buffer1[i] /= max;
		}

	}
	/*
	 * If the impulse is STEREO
	 */
	else if (synth_impulse->numChannels == STEREO) {
		//TODO: Create new exponential fit data based on top_vals and bottom_vals, not on impulse data.
		float **exp_fit_left = getExponentialFitFromGraph(
				synth_impulse->numFrames / FFT_SIZE, LEFT);
		float **exp_fit_right = getExponentialFitFromGraph(
				synth_impulse->numFrames / FFT_SIZE, RIGHT);

		setTopValsBasedOnImpulseFFTBlocks(exp_fit_left, LEFT);
		setTopValsBasedOnImpulseFFTBlocks(exp_fit_right, RIGHT);

		//Then, filter white noise with this exponential fit data.
		float *synthesized_impulse_buffer_left = getFilteredWhiteNoise(
				synth_impulse, exp_fit_left);
		float *synthesized_impulse_buffer_right = getFilteredWhiteNoise(
				synth_impulse, exp_fit_right);

		g_amp_envelope = getAmplitudeEnvelope(synth_impulse, LEFT);

		//Then, apply amp envelope.
		applyAmplitudeEnvelope(synth_impulse, synthesized_impulse_buffer_left,
				g_amp_envelope);
		applyAmplitudeEnvelope(synth_impulse, synthesized_impulse_buffer_right,
				g_amp_envelope);

		// crossfade between recorded impulse attack and synthesized tail
		crossfadeRecordedAndSynthesizedImpulses(synthesized_impulse_buffer_left,
				currentImpulse, LEFT);
		crossfadeRecordedAndSynthesizedImpulses(synthesized_impulse_buffer_right,
				currentImpulse, RIGHT);

		// Write to a wav file
		//		writeWavFile(synthesized_impulse_buffer, SAMPLE_RATE,
		//				synth_impulse->numChannels, synth_impulse->numFrames, 1,
		//				"11_10_2015_test.wav");

		// Put synthesized impulse in audioData struct
		synth_impulse->buffer1 = synthesized_impulse_buffer_left;
		synth_impulse->buffer2 = synthesized_impulse_buffer_right;

		int i;
		float max = 0.0f;
		for (i = 0; i < synth_impulse->numFrames; i++) {
			if (fabsf(synth_impulse->buffer1[i]) > max) {
				max = fabsf(synth_impulse->buffer1[i]);
			}
			if (fabsf(synth_impulse->buffer2[i]) > max) {
				max = fabsf(synth_impulse->buffer2[i]);
			}
		}

		for (i = 0; i < synth_impulse->numFrames; i++) {
			synth_impulse->buffer1[i] /= max;
			synth_impulse->buffer2[i] /= max;
		}
	}

	// Free all data from previous impulse
	free(currentImpulse->buffer1);
	free(currentImpulse->buffer2);
	//	free(g_impulse);
	g_impulse = synth_impulse;

	//Then, recalculate all the stuff in loadImpulse() based on the resynthesized impulse.
	return synth_impulse;
}

/*
 * This function synthesizes an impulse from an audio file.
 */
audioData *synthesizeImpulse(char *fileName) {

	// Preliminary calculations/processes
	audioData *impulse_from_file = fileToBuffer(fileName);
	audioData *synth_impulse = (audioData *) malloc(sizeof(audioData));
	int length_before_zero_padding = impulse_from_file->numFrames;
	zeroPadToNextPowerOfTwo(impulse_from_file);
	int num_impulse_blocks = (impulse_from_file->numFrames / FFT_SIZE);

	int i;

	/*
	 * If the impulse is MONO
	 */
	if (impulse_from_file->numChannels == MONO) {

		float max = 0.0f;
		for (i = 0; i < impulse_from_file->numFrames; i++) {
			if (fabsf(impulse_from_file->buffer1[i]) > max) {
				max = fabsf(impulse_from_file->buffer1[i]);
			}
		}

		for (i = 0; i < impulse_from_file->numFrames; i++) {
			impulse_from_file->buffer1[i] /= max;
		}

		// Get the impulse FFT spectrogram
		float **impulse_filter_env_blocks = getImpulseFFTBlocks(
				impulse_from_file, LEFT);

		/*
		 * For each impulse_filter_env block, create exponential fit (to smooth out filter decay)
		 */
		float **impulse_filter_env_blocks_exp_fit =
				getExponentialFitForImpulseFFTBlocks(num_impulse_blocks,
						length_before_zero_padding, impulse_filter_env_blocks);

		// Use exponential fit data to draw impulse frequency response
		setTopValsBasedOnImpulseFFTBlocks(impulse_filter_env_blocks_exp_fit, LEFT);

		g_amp_envelope = getAmplitudeEnvelope(impulse_from_file, LEFT);

		// Filter white noise with exponential fit FFT data
		float *synthesized_impulse_buffer = getFilteredWhiteNoise(
				impulse_from_file, impulse_filter_env_blocks_exp_fit);

		// Apply the amplitude envelope
		applyAmplitudeEnvelope(impulse_from_file, synthesized_impulse_buffer,
				g_amp_envelope);

		// crossfade between recorded impulse attack and synthesized tail
		crossfadeRecordedAndSynthesizedImpulses(synthesized_impulse_buffer,
				impulse_from_file, LEFT);

		//		// Write to a wav file
		//		writeWavFile(synthesized_impulse_buffer, impulse_from_file->sampleRate,
		//				impulse_from_file->numChannels, impulse_from_file->numFrames, 1,
		//				"11_6_2015_test.wav");

		// Put synthesized impulse in audioData struct
		synth_impulse->buffer1 = synthesized_impulse_buffer;
		synth_impulse->numChannels = impulse_from_file->numChannels;
		synth_impulse->numFrames = impulse_from_file->numFrames;
		synth_impulse->sampleRate = SAMPLE_RATE;
	}
	/*
	 * If the impulse is STEREO
	 */
	else if (impulse_from_file->numChannels == STEREO) {
		float max = 0.0f;
		for (i = 0; i < impulse_from_file->numFrames; i++) {
			if (fabsf(impulse_from_file->buffer1[i]) > max) {
				max = fabsf(impulse_from_file->buffer1[i]);
			}
			if (fabsf(impulse_from_file->buffer2[i]) > max) {
				max = fabsf(impulse_from_file->buffer2[i]);
			}
		}

		for (i = 0; i < impulse_from_file->numFrames; i++) {
			impulse_from_file->buffer1[i] /= max;
			impulse_from_file->buffer2[i] /= max;
		}

		// Get the impulse FFT spectrogram
		float **impulse_filter_env_blocks_left = getImpulseFFTBlocks(
				impulse_from_file, LEFT);
		float **impulse_filter_env_blocks_right = getImpulseFFTBlocks(
				impulse_from_file, RIGHT);

		/*
		 * For each impulse_filter_env block, create exponential fit (to smooth out filter decay)
		 */
		float **impulse_filter_env_blocks_exp_fit_left =
				getExponentialFitForImpulseFFTBlocks(num_impulse_blocks,
						length_before_zero_padding, impulse_filter_env_blocks_left);
		float **impulse_filter_env_blocks_exp_fit_right =
				getExponentialFitForImpulseFFTBlocks(num_impulse_blocks,
						length_before_zero_padding, impulse_filter_env_blocks_right);

		// Use exponential fit data to draw impulse frequency response
		setTopValsBasedOnImpulseFFTBlocks(impulse_filter_env_blocks_exp_fit_left, LEFT);
		setTopValsBasedOnImpulseFFTBlocks(impulse_filter_env_blocks_exp_fit_right, RIGHT);

		g_amp_envelope = getAmplitudeEnvelope(impulse_from_file, LEFT);

		// Filter white noise with exponential fit FFT data
		float *synthesized_impulse_buffer_left = getFilteredWhiteNoise(
				impulse_from_file, impulse_filter_env_blocks_exp_fit_left);
		float *synthesized_impulse_buffer_right = getFilteredWhiteNoise(
				impulse_from_file, impulse_filter_env_blocks_exp_fit_right);

		// Apply the amplitude envelope
		applyAmplitudeEnvelope(impulse_from_file, synthesized_impulse_buffer_left,
				g_amp_envelope);
		applyAmplitudeEnvelope(impulse_from_file, synthesized_impulse_buffer_right,
				g_amp_envelope);

		// crossfade between recorded impulse attack and synthesized tail
		crossfadeRecordedAndSynthesizedImpulses(synthesized_impulse_buffer_left,
				impulse_from_file, LEFT);
		crossfadeRecordedAndSynthesizedImpulses(synthesized_impulse_buffer_right,
				impulse_from_file, RIGHT);

		//		// Write to a wav file
		//		writeWavFile(synthesized_impulse_buffer, impulse_from_file->sampleRate,
		//				impulse_from_file->numChannels, impulse_from_file->numFrames, 1,
		//				"11_6_2015_test.wav");

		// Put synthesized impulse in audioData struct
		synth_impulse->buffer1 = synthesized_impulse_buffer_left;
		synth_impulse->buffer2 = synthesized_impulse_buffer_right;
		synth_impulse->numChannels = impulse_from_file->numChannels;
		synth_impulse->numFrames = impulse_from_file->numFrames;
		synth_impulse->sampleRate = SAMPLE_RATE;
	}

	return synth_impulse;
}

/*
 * This function loads an impulse from a given filename
 */
void loadImpulse(char *name) {
	g_impulse = synthesizeImpulse(name);
	//	impulse = fileToBuffer("churchIR.wav");
//	g_impulse = zeroPadToNextPowerOfTwo(g_impulse);
	g_impulse_length = g_impulse->numFrames;
	g_impulse_num_frames = g_impulse->numFrames;
	Vector blockLengthVector = determineBlockLengths(g_impulse);
	BlockData* data_ptr = allocateBlockBuffers(blockLengthVector, g_impulse);
	partitionImpulseIntoBlocks(blockLengthVector, data_ptr, g_impulse);
	g_fftData_ptr = allocateFFTBuffers(data_ptr, blockLengthVector, g_impulse);
	for (int i=0; i<blockLengthVector.size; i++) {
		free(data_ptr->audioBlocks1[i]);
		free(data_ptr->audioBlocks2[i]);
	}
	free(data_ptr->audioBlocks1);
	free(data_ptr->audioBlocks2);
	free(data_ptr);
	vector_free(&blockLengthVector);
}

/*
 * This function is responsible for generating the block lengths used in the
 * partitioning scheme for real-time convolution.
 */
void initializePowerOf2Vector() {
	vector_init(&g_powerOf2Vector);
	int counter = 0;
	while (pow(2, counter) <= g_max_factor) {
		vector_append(&g_powerOf2Vector, pow(2, counter++));
	}
	//	for (int i=0; i<g_powerOf2Vector.size; i++) {
	//		printf("the powerOf2Vector[%d]: %d\n", i, vector_get(&g_powerOf2Vector, i));
	//	}
}

/*
 * This function reloads an impulse after changes have been made
 */
void reloadImpulse() {
	//	free_audioData(g_impulse);
	g_impulse = resynthesizeImpulse(g_impulse, g_impulse_num_frames);
	g_impulse = zeroPadToNextPowerOfTwo(g_impulse);
	g_impulse_length = g_impulse->numFrames;
	Vector blockLengthVector = determineBlockLengths(g_impulse);
	BlockData* data_ptr = allocateBlockBuffers(blockLengthVector, g_impulse);
	partitionImpulseIntoBlocks(blockLengthVector, data_ptr, g_impulse);
	//	free(g_fftData_ptr);
	g_fftData_ptr = allocateFFTBuffers(data_ptr, blockLengthVector, g_impulse);
	for (int i=0; i<blockLengthVector.size; i++) {
		free(data_ptr->audioBlocks1[i]);
		free(data_ptr->audioBlocks2[i]);
	}
	free(data_ptr->audioBlocks1);
	free(data_ptr->audioBlocks2);
	free(data_ptr);
	vector_free(&blockLengthVector);
	initializeGlobalParameters();
	initializePowerOf2Vector();
}

void setWindowRange() {
	// For displaying impulse
	impulseWindow->edge_margin = 20;
	impulseWindow->margin_top = 120;
	impulseWindow->margin_bottom = g_height - 80;
	impulseWindow->margin_left = 40;
	impulseWindow->margin_right = g_width - 40;
	float range = (float) (impulseWindow->margin_bottom - impulseWindow->margin_top);
	impulseWindow->top_line = (int) (range * impulseWindow->top_line_percent) + impulseWindow->margin_top;
	impulseWindow->mid_line = (int) (range * impulseWindow->mid_line_percent) + impulseWindow->margin_top;
	impulseWindow->bottom_line = (int) (range * impulseWindow->bottom_line_percent) + impulseWindow->margin_top;

	impulseHorizontalSelect->top_left.x = impulseWindow->margin_left;
	impulseHorizontalSelect->top_left.y = impulseWindow->margin_bottom + impulseWindow->edge_margin  + 10;

	impulseHorizontalSelect->top_right.x = impulseWindow->margin_right;
	impulseHorizontalSelect->top_right.y = impulseWindow->margin_bottom + impulseWindow->edge_margin + 10;

	impulseHorizontalSelect->bottom_left.x = impulseWindow->margin_left;
	impulseHorizontalSelect->bottom_left.y = g_height - 10;

	impulseHorizontalSelect->bottom_right.x = impulseWindow->margin_right;
	impulseHorizontalSelect->bottom_right.y = g_height - 10;

	range = (float) (impulseHorizontalSelect->top_right.x - impulseHorizontalSelect->top_left.x);
	impulseHorizontalSelect->left_bound = (int) (range * impulseHorizontalSelect->left_bound_percent) + impulseHorizontalSelect->top_left.x;
	impulseHorizontalSelect->right_bound = (int) (range * impulseHorizontalSelect->right_bound_percent) + impulseHorizontalSelect->top_left.x;
}

void initializeImpulseLengthSlider() {
	int impulse_length_min = 235;
	int impulse_length_max = 325;
	int range = impulse_length_max - impulse_length_min;
	int impulse_num_frames_min = 4410;
	int impulse_num_frames_max = 44100 * 20;
	int impulse_num_frames_range = impulse_num_frames_max
			- impulse_num_frames_min;
	float offset = (float) (g_impulse->numFrames - impulse_num_frames_min)
					/ (float) impulse_num_frames_range;
	int sliderInitPos = offset * ((float) range) + impulse_length_min;
	ImpulseLengthSlider.x_min = 235;
	ImpulseLengthSlider.x_max = 325;
	ImpulseLengthSlider.x_pos = sliderInitPos;
	ImpulseLengthSlider.y = 65;
	ImpulseLengthSlider.min_val = 44100;
	ImpulseLengthSlider.max_val = 44100 * 20;
	ImpulseLengthSlider.current_val = g_impulse->numFrames;
	ImpulseLengthSlider.label = "Length";
	ImpulseLengthSlider.state = 0;
	ImpulseLengthSlider.callbackFunction = ImpulseLengthSliderCallback;
	//	printf("impulse_length: %f seconds\n",
	//			(float) g_impulse->numFrames / SAMPLE_RATE);
}

GraphData *getValuesFromGraph(int current_channel) {
	//TODO: manipulate values that are in the selected range
	int first_index = 0;
	int last_index = 0;
	bool started = false;
	bool finished = false;
	for (int i=0; i<HALF_FFT_SIZE; i++) {
		if (impulseWindow->margin_left + x_values[i] > impulseHorizontalSelect->left_bound && impulseWindow->margin_left + x_values[i] < impulseHorizontalSelect->right_bound) {
			if (started == false) {
				first_index = i;
				started = true;
			}
		} else if (started && !finished) {
			last_index = i-1;
			finished = true;
		}
	}
	if (last_index - first_index == 0) {
		return NULL;
	}

	GraphData *graphData = (GraphData *) malloc(sizeof(GraphData));
	graphData->first_index = first_index;
	graphData->last_index = last_index;
	int length = last_index - first_index + 1;
	graphData->length = length;
	graphData->x_indices = (int *) malloc(sizeof(int) * length);
	graphData->y_values = (float *) malloc(sizeof(float) * length);

	if (current_channel == LEFT) {
		for (int i=0; i<length; i++) {
			graphData->x_indices[i] = x_values[graphData->first_index + i];
			graphData->y_values[i] = top_vals_left[graphData->first_index + i];
		}
	} else if (current_channel == RIGHT) {
		for (int i=0; i<length; i++) {
			graphData->x_indices[i] = x_values[graphData->first_index + i];
			graphData->y_values[i] = top_vals_right[graphData->first_index + i];
		}
	}

	return graphData;
}

/*
 * Main function.
 */
int main(int argc, char **argv) {

	srand(time(NULL));

	for (int i=0; i<HALF_FFT_SIZE; i++) {
		for (int j=0; j<16; j++) {
			last_input_spectrum[j][i] = 0.0f;
		}
	}

	input_spectrum = (complex *) malloc(MIN_FFT_BLOCK_SIZE * sizeof(complex));

	impulseWindow = (ImpulseWindow *) malloc(sizeof(ImpulseWindow));
	impulseHorizontalSelect = (ImpulseHorizontalSelect *) malloc(sizeof(ImpulseHorizontalSelect));

	impulseHorizontalSelect->left_bound_percent = 0.33;
	impulseHorizontalSelect->right_bound_percent = 0.66;
	impulseHorizontalSelect->left_bound_selected = false;
	impulseHorizontalSelect->right_bound_selected = false;

	impulseWindow->top_line_percent = 0.25;
	impulseWindow->mid_line_percent = 0.50;
	impulseWindow->bottom_line_percent = 0.75;
	impulseWindow->top_line_selected = false;
	impulseWindow->mid_line_selected = false;
	impulseWindow->bottom_line_selected = false;


	setWindowRange();

	loadImpulse(IMPULSE_FILE_NAME);

	audio_file_name = AUDIO_FILE_NAME;

	initializeImpulseLengthSlider();

	ChannelSlider.x_min = 355;
	ChannelSlider.x_max = 445;
	ChannelSlider.x_pos = 355;
	ChannelSlider.y = 65;
	ChannelSlider.min_val = 1;
	ChannelSlider.max_val = 3;
	ChannelSlider.current_val = LEFT;
	ChannelSlider.label = "Channel";
	ChannelSlider.state = 0;
	ChannelSlider.callbackFunction = ChannelSliderCallback;

	initialize_glut(argc, argv);

	initializeGlobalParameters();

	initializePowerOf2Vector();

	runPortAudio();

	return 0;
}

