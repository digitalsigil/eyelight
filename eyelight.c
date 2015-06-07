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

jack_port_t *in_port;
jack_port_t *out_ports[2];
jack_default_audio_sample_t ramp = 0.0;
jack_default_audio_sample_t note_on;
unsigned char note = 0;
jack_default_audio_sample_t note_frqs[128];

jack_client_t *client;

enum {
	BlockSampleCount = 1 << 10,
	BlockCount = 100
};

struct blocks {
	float **b;
	int n;
	int m;
} blocks;

static void
signal_handler(int sig)
{
	jack_client_close(client);
	printf("signal received, exiting ...\n");
	exit(0);
}

static void
fill_random(float *b, int n)
{
	static gsl_rng *r = NULL;
	int i;
	float rf;
	double *d = NULL;
	
	if (r == NULL) {
		gsl_rng_env_setup();
		r = gsl_rng_alloc(gsl_rng_default);
	}

	d = malloc(sizeof (*d) * n);
	if (!d) errx(1, "malloc");

	for (i = 0; i < n; i++)
		d[i] = 2 * (gsl_rng_uniform(r) - 0.5);
	//rf = gsl_rng_uniform(r);
	//for (i = 0; i < n; i++)
	//	b[i] = sinf((440.0 + rf * 2000 - 1000) * M_PI * 2 * i / n);
	
	gsl_fft_halfcomplex_radix2_inverse (d, 1, n);

	for (i = 0; i < n; i++)
		b[i] = d[i];
	
	free(d);
}

static struct blocks
create_blocks(int m, int n)
{
	int i;
	struct blocks b;
	
	b.m = m;
	b.n = n;
	b.b = malloc (sizeof (*b.b) * m);
	if (!b.b) errx(1, "malloc");
	
	for (i = 0; i < m; i++) {
		b.b[i] = malloc(sizeof (**b.b) * n);
		if (b.b[i] == NULL) errx(1, "malloc");
		fill_random(b.b[i], n);
	}

	return b;
}

static int
block_fade(float *out, int i0, int i1, int o0, int o1, int n)
{
	double a;
	int i;
	
	float *b0 = blocks.b[i0] + o0;
	float *b1 = blocks.b[i1] + o1;

	if (n > blocks.n - o0) n = blocks.n - o0;
	if (n > blocks.n - o1) n = blocks.n - o1;

	for (i = 0; i < n; i++) {
		a = M_PI * o0 / blocks.n;
		out[i] = sin(a) * b0[i] + cos(a) * b1[i];
	}

	return n;
}

static void
blockone_fill(float *l, float *r, int n)
{
	static int o = 0;
	int i;
	
	for (i = 0; i < n; i++) {
		l[i] = r[i] = blocks.b[0][o];
		o = (o + 1) % blocks.n;
	}
}

static void
noise_fill(float *l, float *r, int n)
{
	static int i0 = 0, i1 = 0;
	static int o0 = -1, o1 = 0;
	int rv;

	if (o0 == -1) o0 = blocks.n / 2;

	while (n) {
		rv = block_fade(l, i0, i1, o0, o1, n);
		memcpy(r, l, sizeof (*l) * rv);
	
		o0 += rv;
		o1 += rv;
		n -= rv;

		if (o0 >= blocks.m) {
			o0 = 0;
			i0 = random() % blocks.m;
		}

		if (o1 >= blocks.m) {
			o1 = 0;
			i1 = random() % blocks.m;
		}
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
process_callback(jack_nframes_t nframes, void *arg)
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

	//blockone_fill(out[0], out[1], nframes);
	noise_fill(out[0], out[1], nframes);

	return 0;
}

static int
srate_callback(jack_nframes_t nframes, void *arg)
{
	printf("the sample rate is now %" PRIu32 "/sec\n", nframes);
	calc_note_frqs((jack_default_audio_sample_t) nframes);
	return 0;
}

static void
shutdown_callback(void *arg)
{
	errx(1, "JACK shut down, exiting ...\n");
}

int
main(int narg, char **args)
{
	client = jack_client_open("eyelight", JackNullOption, NULL);
	if (client == 0)
		errx(1, "JACK server not running?\n");

	calc_note_frqs(jack_get_sample_rate(client));
	blocks = create_blocks(BlockCount, BlockSampleCount);
	
	jack_set_process_callback(client, process_callback, 0);
	jack_set_sample_rate_callback(client, srate_callback, 0);
	jack_on_shutdown(client, shutdown_callback, 0);

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
