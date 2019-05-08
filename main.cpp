	/**
 * Final Project of Digital Audio Processing
 *
 * Objectives:
 * 1- Estimation of the angle of arrival of a sound source by generalized cross-correlation
 * 2- Sound source separation (spatial filtering) by beamforming
 *
 * Author: 		Luis Miguel Gato Diaz 		e-mail: lmiguelgato@gmail.com
 * Professor:	Caleb Rascon Estebane		e-mail: caleb.rascon@gmail.com
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tools/max.h"
#include "tools/angleDictionary.h"

// JACK: professional sound server daemon that provides real-time, 
//       low-latency connections for both audio and MIDI data between applications that use its API.
#include <jack/jack.h>

// FFTW: library for computing the discrete Fourier transform of arbitrary input size, 
//       and of both real and complex data.
#include <complex.h> 	// needs to be included before fftw3.h for compatibility
#include <fftw3.h>

// Eigen: C++ template library for linear algebra: matrices, vectors, numerical solvers, and related algorithms.
#include <Eigen/Eigen>

#define RAD2DEG 57.295779513082323f

// JACK:
jack_port_t **input_ports;
jack_port_t **output_ports;
jack_client_t *client;

// FFTW:
std::complex<double> *i_fft_4N, *i_time_4N, *o_fft_4N, *o_time_4N;
std::complex<double> *i_fft_2N, *i_time_2N, *o_fft_2N, *o_time_2N;
fftw_plan i_forward_4N, o_inverse_4N, i_forward_2N, o_inverse_2N;

// default parameters:
double sample_rate  = 48000.0;			// default sample rate
int nframes 		= 1024;				// default number of frames per jack buffer
int window_size 	= 4096;				// default fft size (window_size must be four times nframes)
double mic_separation = 0.1;			// default microphone separation [meters]
double c = 343.364;						// default sound speed [m/s]
double dt_max = mic_separation/c;		// maximum delay between microphones [s]

unsigned int n_out_channels = 2;		// number of output channels
unsigned int n_in_channels = 3;			// number of input channels

jack_default_audio_sample_t *hann;		// array to store the Hann window

// overlap-add registers:
jack_default_audio_sample_t **X_full;	// store the 6 last buffers of 'nframes' samples
jack_default_audio_sample_t **X_late;	// store the 4 latest buffers from X_full
jack_default_audio_sample_t **X_early;	// store the 4 earliest buffers from X_full

// GCC registers:
std::complex<double> **X_gcc;			// store GCC result (length 2 times 'nframes')
std::complex<double> *Aux_gcc;			// store axuliary GCC result (length 2 times 'nframes')

/**
 * The process callback for this JACK application is called in a
 * special realtime thread once for each audio cycle.
 *
 * This client does nothing more than copy data from its input
 * port to its output port. It will exit when stopped by 
 * the user (e.g. using Ctrl-C on a unix-ish operating system)
 */
int jack_callback (jack_nframes_t nframes, void *arg){

	int i, j, k;

	jack_default_audio_sample_t **in;
	jack_default_audio_sample_t **out;

	in = (jack_default_audio_sample_t **)malloc(n_in_channels*sizeof(jack_default_audio_sample_t *));
	for(i = 0; i < n_in_channels; ++i)
		in[i] = (jack_default_audio_sample_t *)jack_port_get_buffer (input_ports[i], nframes);

	out = (jack_default_audio_sample_t **)malloc(n_out_channels*sizeof(jack_default_audio_sample_t *));
	for(i = 0; i < n_out_channels; ++i)
		out[i] = (jack_default_audio_sample_t *)jack_port_get_buffer (output_ports[i], nframes);

	for (j = 0; j < n_in_channels; ++j) {

		// shift-register (useful for overlap-add method):
		for (i = 0; i < nframes; ++i)
		{
			X_full[j][i] 				= X_full[j][nframes + i];
			X_full[j][nframes + i] 		= X_full[j][2*nframes + i];
			X_full[j][2*nframes + i] 	= X_full[j][3*nframes + i];
			X_full[j][3*nframes + i] 	= X_full[j][4*nframes + i];
			X_full[j][4*nframes + i] 	= X_full[j][5*nframes + i];
			X_full[j][5*nframes + i] 	= in[j][i];	
		}

		// cross-correlation in four steps:
		// 1- zero padding:
		for (i = 0; i < nframes; ++i)
		{
			i_time_2N[i] = X_full[j][i];
		}
		for (i = nframes; i < 2*nframes; ++i)
		{
			i_time_2N[i] = 0.0;
		}
		// 2- apply FFT:
		fftw_execute(i_forward_2N);
		for (i = 0; i < 2*nframes; ++i)
		{
			X_gcc[j][i] = i_fft_2N[i];
		}
	}

	// 3- multiply pairs of FFTs (time reversing one them), and
	// 4- apply iFFT
	for (i = 0; i < 2*nframes; ++i)
	{
		o_fft_2N[i] = X_gcc[0][i] * conj(X_gcc[1][i]);
	}
	fftw_execute(o_inverse_2N);
	for (i = 0; i < 2*nframes; ++i)
	{
		Aux_gcc[i] = o_time_2N[i];
	}

	for (i = 0; i < 2*nframes; ++i)
	{
		o_fft_2N[i] = X_gcc[1][i] * conj(X_gcc[2][i]);
	}
	fftw_execute(o_inverse_2N);
	for (i = 0; i < 2*nframes; ++i)
	{
		X_gcc[1][i] = Aux_gcc[i];
		Aux_gcc[i] = o_time_2N[i];
	}

	for (i = 0; i < 2*nframes; ++i)
	{
		o_fft_2N[i] = X_gcc[2][i] * conj(X_gcc[0][i]);
	}
	fftw_execute(o_inverse_2N);
	for (i = 0; i < 2*nframes; ++i)
	{
		X_gcc[2][i] = Aux_gcc[i];
		X_gcc[0][i] = o_time_2N[i];
	}

	// find maximum of the cross-correlation:

	int max_idx12, max_idx23, max_idx31;
	double max_val12, max_val23, max_val31;

	max_idx12 = max(X_gcc[1], window_size, &max_val12);
	max_idx23 = max(X_gcc[2], window_size, &max_val23);
	max_idx31 = max(X_gcc[0], window_size, &max_val31);

	double dt12, dt23, dt31;

	if (max_idx12 < nframes)
		dt12 = ((double) max_idx12)/sample_rate;
	else
		dt12 = ((double) 2*nframes - max_idx12)/sample_rate;

	if (max_idx23 < nframes)
		dt23 = ((double) max_idx23)/sample_rate;
	else
		dt23 = ((double) 2*nframes - max_idx23)/sample_rate;

	if (max_idx31 < nframes)
		dt31 = ((double) max_idx31)/sample_rate;
	else
		dt31 = ((double)  2*nframes - max_idx31)/sample_rate;

	if (dt12 > dt_max)
		dt12 = dt_max;
	if (dt12 < -dt_max)
		dt12 = -dt_max;

	if (dt23 > dt_max)
		dt23 = dt_max;
	if (dt23 < -dt_max)
		dt23 = -dt_max;

	if (dt31 > dt_max)
		dt31 = dt_max;
	if (dt31 < -dt_max)
		dt31 = -dt_max;

	double theta[6] = {asin(dt12/dt_max)*RAD2DEG,
					   asin(dt23/dt_max)*RAD2DEG + 120.0,
					   asin(dt31/dt_max)*RAD2DEG - 120.0,
					   0.0,
					   0.0,
					   0.0};

	for (i = 0; i < 3; ++i) {
		if (theta[i] >= 180.0) {
			theta[i] -= 360.0;
		}
		if (theta[i] < -180.0) {
			theta[i] += 360.0;
		}

		if (theta[i] <= 0.0) {
			theta[i+3] = 180.0 + theta[i];
		}
		if (theta[i] > 0.0) {
			theta[i+3] = theta[i] - 180.0;
		}		
	}

	//angleDictionary (theta);

	printf("theta1 = [%1.5f, %1.5f];\ttheta2 = [%1.5f, %1.5f];\ttheta3 = [%1.5f, %1.5f]\n", theta[0], theta[3], theta[1], theta[4], theta[2], theta[5]);


	//printf("theta1 = %1.6f\t theta2 = %1.6f\t theta3 = %1.6f\n", theta12, theta23, theta31);

	// perform overlap-add:
	for (k = 0; k < n_out_channels; ++k) {
		for (i = 0; i < nframes; ++i) {
			out[k][i] = 0;			
			for (j = 0; j < n_in_channels; ++j)
			{
				out[k][i] += in[j][i];
			}
		}
	}
	
	return 0;
}


/**
 * JACK calls this shutdown_callback if the server ever shuts down or
 * decides to disconnect the client.
 */
void jack_shutdown (void *arg){
	exit (1);
}


int main (int argc, char *argv[]) {

	int i;

	if(argc != 2){
		printf ("Microphone separation not provided.\n");
		exit(1);
	}

	mic_separation = atof(argv[1]);
	printf("\nMicrophone separation: %1.4f meters\n\n", mic_separation);	

	dt_max = mic_separation/c;

	const char *client_name = "jack_doa_beamformer";
	jack_options_t options = JackNoStartServer;
	jack_status_t status;
	
	/* open a client connection to the JACK server */
	client = jack_client_open (client_name, options, &status);
	if (client == NULL){
		/* if connection failed, say why */
		printf ("jack_client_open() failed, status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			printf ("Unable to connect to JACK server.\n");
		}
		exit (1);
	}
	
	/* if connection was successful, check if the name we proposed is not in use */
	if (status & JackNameNotUnique){
		client_name = jack_get_client_name(client);
		printf ("Warning: other agent with our name is running, `%s' has been assigned to us.\n", client_name);
	}
	
	/* tell the JACK server to call 'jack_callback()' whenever there is work to be done. */
	jack_set_process_callback (client, jack_callback, 0);
	
	/* tell the JACK server to call 'jack_shutdown()' if it ever shuts down,
	   either entirely, or if it just decides to stop calling us. */
	jack_on_shutdown (client, jack_shutdown, 0);

	// obtain here the delay from user and store it in 'delay' 
	nframes 	= (int) jack_get_buffer_size (client);
	window_size = 4*nframes;

	// initialization of internal buffers
	// - overlap-add buffers
	X_late	= (jack_default_audio_sample_t **) calloc(n_in_channels, sizeof(jack_default_audio_sample_t*));
	X_early	= (jack_default_audio_sample_t **) calloc(n_in_channels, sizeof(jack_default_audio_sample_t*));
	X_full	= (jack_default_audio_sample_t **) calloc(n_in_channels, sizeof(jack_default_audio_sample_t*));
	X_gcc	= (std::complex<double> **) calloc(n_in_channels, sizeof(std::complex<double>*));
	Aux_gcc	= (std::complex<double> *) calloc(window_size/2, sizeof(std::complex<double>));

    for (i = 0; i < n_in_channels; ++i) {
        X_late[i]	= (jack_default_audio_sample_t *) calloc(window_size, sizeof(jack_default_audio_sample_t));
		X_early[i]	= (jack_default_audio_sample_t *) calloc(window_size, sizeof(jack_default_audio_sample_t));
		X_full[i]	= (jack_default_audio_sample_t *) calloc(window_size + window_size/2, sizeof(jack_default_audio_sample_t));
		X_gcc[i]	= (std::complex<double> *) calloc(window_size/2, sizeof(std::complex<double>));
    }	

	// - FFTW3 buffers
	i_fft_4N = (std::complex<double>*) fftw_malloc(sizeof(std::complex<double>) * window_size);
	i_time_4N = (std::complex<double>*) fftw_malloc(sizeof(std::complex<double>) * window_size);
	o_fft_4N = (std::complex<double>*) fftw_malloc(sizeof(std::complex<double>) * window_size);
	o_time_4N = (std::complex<double>*) fftw_malloc(sizeof(std::complex<double>) * window_size);
	
	i_forward_4N = fftw_plan_dft_1d(window_size, reinterpret_cast<fftw_complex*>(i_time_4N), reinterpret_cast<fftw_complex*>(i_fft_4N), FFTW_FORWARD, FFTW_MEASURE);
	o_inverse_4N = fftw_plan_dft_1d(window_size, reinterpret_cast<fftw_complex*>(o_fft_4N), reinterpret_cast<fftw_complex*>(o_time_4N), FFTW_BACKWARD, FFTW_MEASURE);

	i_fft_2N = (std::complex<double>*) fftw_malloc(sizeof(std::complex<double>) * window_size/2);
	i_time_2N = (std::complex<double>*) fftw_malloc(sizeof(std::complex<double>) * window_size/2);
	o_fft_2N = (std::complex<double>*) fftw_malloc(sizeof(std::complex<double>) * window_size/2);
	o_time_2N = (std::complex<double>*) fftw_malloc(sizeof(std::complex<double>) * window_size/2);

	i_forward_2N = fftw_plan_dft_1d(window_size/2, reinterpret_cast<fftw_complex*>(i_time_2N), reinterpret_cast<fftw_complex*>(i_fft_2N), FFTW_FORWARD, FFTW_MEASURE);
	o_inverse_2N = fftw_plan_dft_1d(window_size/2, reinterpret_cast<fftw_complex*>(o_fft_2N), reinterpret_cast<fftw_complex*>(o_time_2N), FFTW_BACKWARD, FFTW_MEASURE);
	
	sample_rate = (double) jack_get_sample_rate(client);

	// - hann window
	hann = (jack_default_audio_sample_t *) calloc(window_size, sizeof(jack_default_audio_sample_t)); 
	for(i = 0; i < window_size; ++i) {
		hann[i] = 0.5 - 0.5*cos(2.0*M_PI* ((double) i/(window_size-1)));
	}
	
	/* display the current sample rate. */
	printf ("JACK client info:\n");
	printf ("\tEngine sample rate: %d\n", jack_get_sample_rate (client));
	printf ("\tWindow size: %d\n\n", jack_get_buffer_size (client));
	
	char portname[13];
	output_ports = (jack_port_t**) malloc(n_out_channels*sizeof(jack_port_t*));
	for(i = 0; i < n_out_channels; ++i) {
		sprintf(portname, "speaker_%d", i+1);
		output_ports[i] = jack_port_register (client, portname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		if (output_ports[i] == NULL) {
			printf("No more JACK ports available after creating output port number %d\n",i);
			exit (1);
		}
	}	

	input_ports = (jack_port_t**) malloc(n_in_channels*sizeof(jack_port_t*));
	for(i = 0; i < n_in_channels; ++i) {
		sprintf(portname, "wav_mic%d", i+1);
		input_ports[i] = jack_port_register (client, portname, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
		if (input_ports[i] == NULL) {
			printf("No more JACK ports available after creating input port number %d\n",i);
			exit (1);
		}
	}	
	
	/* Tell the JACK server that we are ready to roll.
	   Our jack_callback() callback will start running now. */
	if (jack_activate (client)) {
		printf ("Cannot activate client.");
		exit (1);
	}
	
	printf ("Agent activated.\n");
	
	/* Connect the ports.  You can't do this before the client is
	 * activated, because we can't make connections to clients
	 * that aren't running.  Note the confusing (but necessary)
	 * orientation of the driver backend ports: playback ports are
	 * "input" to the backend, and capture ports are "output" from
	 * it.
	 */
	printf ("Connecting ports... ");

	const char **serverports_names;
	
	/* Assign our output port to a server input port*/
	// Find possible input server port names
	serverports_names = jack_get_ports (client, NULL, NULL, JackPortIsPhysical|JackPortIsInput);
	if (serverports_names == NULL) {
		printf("No available physical playback (server input) ports.\n");
		exit (1);
	}
	for(i = 0; i<n_out_channels; ++i) {
		// Connect the first available to our output port
		if (jack_connect (client, jack_port_name (output_ports[i]), serverports_names[i])) {
			printf ("Cannot connect input port number %d.\n", i);
			exit (1);
		}
	}
	// free serverports_names variable, we're not going to use it again
	free (serverports_names);

	/*serverports_names = jack_get_ports (client, NULL, NULL, JackPortIsPhysical|JackPortIsOutput);
	if (serverports_names == NULL) {
		printf("No available physical capture (server output) ports.\n");
		exit (1);
	}
	// Connect the first available to our input port
	for(i = 0; i<n_in_channels; ++i) {
		// Connect the first available to our output port
		if (jack_connect (client, serverports_names[i], jack_port_name (input_ports[i]))) {
			printf ("Cannot connect output port number %d.\n", i);
			exit (1);
		}
	}
	// free serverports_names variable for reuse in next part of the code
	free (serverports_names);	*/
	
	printf ("done.\n\n");
	/* keep running until stopped by the user */
	sleep (-1);
	
	
	/* this is never reached but if the program
	   had some other way to exit besides being killed,
	   they would be important to call.
	*/
	jack_client_close (client);
	exit (0);
}