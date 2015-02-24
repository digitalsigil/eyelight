#include <stdio.h>
#include <errno.h>
#include <err.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <inttypes.h>

#include <gsl/gsl_rng.h>
#include <gsl/gsl_fft_halfcomplex.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#define BLOCK_N 20
#define BLOCK_SMP (1 << 16)

jack_port_t *in_port;
jack_port_t *out_ports[2];
jack_default_audio_sample_t ramp = 0.0;
jack_default_audio_sample_t note_on;
unsigned char note = 0;
jack_default_audio_sample_t note_frqs[128];

jack_client_t *client;

double *block[BLOCK_N];


static void
signal_handler(int sig)
{
	jack_client_close(client);
	printf("signal received, exiting ...\n");
	exit(0);
}

static void
fill_block(float *b, int n)
{
	static gsl_rng *r = NULL;

	int i;

	if (r == NULL) {
		gsl_rng_env_setup();
		r = gsl_rng_alloc(gsl_rng_default);
	}

	for (i = 0; i < n; i++)
		b[i] = 2.6 * (((double) i) / n) * gsl_rng_uniform(r);
		
	gsl_fft_halfcomplex_radix2_inverse (b, 1, n);
}

static void
generate_blocks(void)
{
	int i;
	
	for (i = 0; i < BLOCK_N; i++) {
		block[i] = malloc(sizeof (**block) * BLOCK_SMP);
		if (block[i] == NULL)
			errx(1, "malloc failed");
		fill_block(block[i], BLOCK_SMP);
	}
}

static void
noise_fill(jack_default_audio_sample_t *b1, jack_default_audio_sample_t *b2, int n)
{
	static int ba = 0, bb = 1;
	static int pos = 0;

	double a;
	int i = 0;
	
	for (i = 0; i < n; i++) {
		if (pos++ >= BLOCK_SMP) {
			ba = bb;
			bb = random() % BLOCK_N;
			pos = 0;
		}
		a = ((double) pos++) / BLOCK_SMP;
		
		b1[i] = sin(a * 0.00001) * block[ba][pos];
		b1[i] = cos(a * 0.00001) * block[bb][pos];
		b2[i] = b1[i];
	}
}

static void
calc_note_frqs(jack_default_audio_sample_t srate)
{
	int i;
	double w;
	jack_default_audio_sample_t j;
	
	w = 2.0 * 440.0 / 32.0;
	for(i=0; i<128; i++) {
		j = (jack_default_audio_sample_t) i;
		note_frqs[i] = w * pow(2, (j - 9.0) / 12.0) / srate;
	}
}

static int
rng_fill(float *d, int n)
{
	static gsl_rng *r = NULL;
	
	int i;

	if (r == NULL) {
		gsl_rng_env_setup();
		r = gsl_rng_alloc(gsl_rng_default);
	}

	for (i = 0; i < n; i++)
	     d[i] = 0.1 * gsl_rng_uniform(r);

	//gsl_rng_free(r);
}

static void
process_midi_event(jack_midi_event_t *me)
{
	jack_midi_data_t *b;
	
	b = me->buffer;
	switch (*b & 0xf0) {
	case 0x90: /* note on */
		note = *(b + 1);
		if (*(b + 2) == 0)
			note_on = 0.0;
		else
			note_on = (float) *(b + 2) / 127.f / 2.f;
		break;
	case 0x80: /* note off */
		note = *(b + 1);
		note_on = 0.0;
		break;
	}
}

static void
process_midi_events(jack_nframes_t nframes)
{
	int i, n;
	void *mibuf;

	mibuf = jack_port_get_buffer(in_port, nframes);
	n = jack_midi_get_event_count(mibuf);

}

static int
process(jack_nframes_t nframes, void *arg)
{
	int i, n;
	void *mibuf;
	jack_default_audio_sample_t *out[2];
	jack_midi_event_t in_event;
	jack_nframes_t event_index = 0;

	mibuf = jack_port_get_buffer(in_port, nframes);
	out[0] = jack_port_get_buffer(out_ports[0], nframes);
	out[1] = jack_port_get_buffer(out_ports[1], nframes);
	n = jack_midi_get_event_count(mibuf);

	/*
	while (n-- > 0) {
		jack_midi_event_get(&in_event, mibuf, 0);
		
		for(i = 0; i < nframes; i++) {
			if ((in_event.time == i) && (event_index < event_count)) {
				process_midi_event(&in_event);
				event_index++;
				if (event_index < n)
					jack_midi_event_get(&in_event,
					    mibuf, event_index);
			}
		ramp += note_frqs[note];
		ramp = (ramp > 1.0) ? ramp - 2.0 : ramp;
		out[0][i] = note_on * sin(2 * M_PI * ramp);
		out[1][i] = note_on * sin(2 * M_PI * ramp);
	}
	*/
	//rng_fill(out[0], nframes);
	//rng_fill(out[1], nframes);
	noise_fill(out[0], out[1], nframes);
	/* for (i = 0; i < nframes; i++) { */
	/* 	out[0][i] = 0.01 * sin(i); */
	/* 	out[1][i] = 0.01 * sin(i); */
	/* } */
	return 0;
}

static int
srate(jack_nframes_t nframes, void *arg)
{
	printf("the sample rate is now %" PRIu32 "/sec\n", nframes);
	calc_note_frqs((jack_default_audio_sample_t) nframes);
	return 0;
}

static void
jack_shutdown(void *arg)
{
	errx(1, "JACK shut down, exiting ...\n");
}

int
main(int narg, char **args)
{
	client = jack_client_open("eyelight", JackNullOption, NULL);
	if (client == 0)
		errx(1, "JACK server not running?\n");

	calc_note_frqs(jack_get_sample_rate (client));
	generate_blocks();
	
	jack_set_process_callback(client, process, 0);
	jack_set_sample_rate_callback(client, srate, 0);
	jack_on_shutdown(client, jack_shutdown, 0);

	in_port = jack_port_register(client, "midi_in",
	    JACK_DEFAULT_MIDI_TYPE,
	    JackPortIsInput, 0);
	out_ports[0] = jack_port_register(client, "audio_l",
	    JACK_DEFAULT_AUDIO_TYPE,
	    JackPortIsOutput, 0);
	out_ports[1] = jack_port_register(client, "audio_r",
	    JACK_DEFAULT_AUDIO_TYPE,
	    JackPortIsOutput, 0);

	if (jack_activate (client))
		errx(1, "cannot activate client");

	/* install a signal handler to properly quits jack client */
	signal(SIGQUIT, signal_handler);
	signal(SIGHUP, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGINT, signal_handler);

	/* run until interrupted */
	while(1) sleep(1);

	jack_client_close(client);
	exit (0);
}
