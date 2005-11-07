#include <inc/lib.h>
#include <lib/libmad/bit.h>
#include <lib/libmad/config.h>
#include <lib/libmad/decoder.h>

struct buffer {
	unsigned char const *start;
	unsigned long length;
};

static
enum mad_flow input(void *data,
		struct mad_stream *stream)
{
	struct buffer *buffer = data;

	if (!buffer->length)
		return MAD_FLOW_STOP;

	mad_stream_buffer(stream, buffer->start, buffer->length);

	buffer->length = 0;

	return MAD_FLOW_CONTINUE;
}

static inline
signed int scale(mad_fixed_t sample)
{
	/* round */
	sample += (1L << (MAD_F_FRACBITS - 16));

	/* clip */
	if (sample >= MAD_F_ONE)
		sample = MAD_F_ONE - 1;
	else if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;

	/* quantize */
	return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static
enum mad_flow output(void *data,
		struct mad_header const *header,
		struct mad_pcm *pcm)
{
	unsigned int nchannels, nsamples;
	mad_fixed_t const *left_ch, *right_ch;

	/* pcm->samplerate contains the sampling frequency */

	nchannels = pcm->channels;
	nsamples  = pcm->length;
	left_ch   = pcm->samples[0];
	right_ch  = pcm->samples[1];

	while (nsamples--) {
		signed int sample;

		/* output sample(s) in 16-bit signed little-endian PCM */

		sample = scale(*left_ch++);
		putchar((sample >> 0) & 0xff);
		putchar((sample >> 8) & 0xff);

		if (nchannels == 2) {
			sample = scale(*right_ch++);
			putchar((sample >> 0) & 0xff);
			putchar((sample >> 8) & 0xff);
		}
	}

	return MAD_FLOW_CONTINUE;
}

void umain(int argc, char **argv)
{
	struct buffer buffer;
	struct mad_decoder decoder;
	int fd, r, i;
	char * in;
	struct Stat st;

	if(argc != 2)
	{
		printf("Usage: %s filename\n", argv[0]);
		return;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0)
	{
		printf("Can't open %s\n", argv[1]);
		return;
	}

	r = fstat(fd, &st);
	if (r < 0)
	{
		printf("fstat() error\n");
		return;
	}

	in = malloc(st.st_size);
	if (!in)
	{
		printf("malloc() error\n");
		return;
	}

	r = read(fd, in, st.st_size);
	if (r != st.st_size)
	{
		printf("read() error\n");
		goto maddec_done;
	}

	// Allocate more pages for the stack because we WILL need it
	for(i = 2; i != 33; i++)
	{
		r = sys_page_alloc(0, (void *) (USTACKTOP - i * PGSIZE), PTE_U |
				PTE_W | PTE_P);
		assert(r >= 0);
	}
	
	buffer.start  = in;
	buffer.length = st.st_size;

	/* configure input, output, and error functions */

	mad_decoder_init(&decoder, &buffer,
			input, 0 /* header */, 0 /* filter */, output,
			0, 0 /* message */);

	/* start decoding */

	mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);

	/* release the decoder */

	mad_decoder_finish(&decoder);

maddec_done:
	free(in);
}
