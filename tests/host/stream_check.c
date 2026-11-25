/* SPDX-License-Identifier: GPL-2.0 */
/*
 * stream_check - the stream layer on the host (developer-guide.md section 4.3):
 * an Annex B stream run through it as the session runs it, its pictures in
 * decoding order and its output order checked.
 *
 *   stream_check [--frames N] [--dropped N] [--verbose] file.h265
 *   stream_check --expect-refusal file.h265
 *
 * The output order: no picture shown twice or shown although not for
 * output, POCs ascending within a coded video sequence, N pictures in all
 * when --frames says how many FFmpeg showed.  No picture dropped, or N
 * when --dropped says how many a damaged stream is made to lose.  --verbose prints every SPS
 * held with its conformance window (make-conformance-refs.sh places the
 * hash window by it) and every picture's type, POC and DPB
 * (break-fixtures.py reads them).  Exit 0 when everything agrees.  With
 * --expect-refusal the stream must be refused at a parameter set (architecture.md
 * section 11, "unsupported"): the unit and the reason are printed, and the
 * exit is 0 when it was; a stream refused in the other mode exits 3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nal.h"
#include "stream.h"

#define UNIT_MAX (1u << 20)

static int verbose;
static struct rp_stream stream;
static struct rp_record record;
static unsigned pictures;    /* the pictures completed */
static unsigned dropped;     /* the pictures dropped: each once, not again for its slices after the one that dropped it */
static unsigned last_number; /* the highest picture number completed: a dropped picture takes a number too */
static unsigned units;      /* NAL units given to the stream so far */
static const char *refused; /* the stream was refused: why */
static unsigned refused_at, refused_type;
static enum rp_ps_result refused_code;

/* the output order */
#define OUT_MAX 4096
static unsigned out_numbers[OUT_MAX];
static unsigned out_count;
static int32_t our_poc[OUT_MAX + 1]; /* by picture number */
static unsigned our_cvs[OUT_MAX + 1]; /* the coded video sequence the picture is in, from 1 */
static BOOL our_output[OUT_MAX + 1];
static unsigned cvs_count;

/**
 * drain_output() - Take every picture the DPB shows next and record it in the output order.
 *
 * Printed with --verbose.
 */
static void drain_output(void)
{
	for (;;)
	{
		unsigned n = rp_dpb_output_next(&stream.dpb);
		if (n == 0)
			break;
		if (verbose)
			printf("  shown: picture %u POC %d\n", n, n <= OUT_MAX ? our_poc[n] : 0);
		if (out_count < OUT_MAX)
			out_numbers[out_count++] = n;
	}
}

/**
 * print_dpb() - Print a picture's DPB array, as POCs with L for long-term, and its three Curr lists, as indices into it (--verbose).
 * @d: the picture's decode parameters
 */
static void print_dpb(const struct v4l2_ctrl_hevc_decode_params *d)
{
	unsigned i;
	printf("[");
	for (i = 0; i < d->num_active_dpb_entries; i++)
		printf("%s%d%s", i ? " " : "", d->dpb[i].pic_order_cnt_val, d->dpb[i].flags ? "L" : "");
	printf("] curr b[");
	for (i = 0; i < d->num_poc_st_curr_before; i++)
		printf("%s%u", i ? " " : "", d->poc_st_curr_before[i]);
	printf("] a[");
	for (i = 0; i < d->num_poc_st_curr_after; i++)
		printf("%s%u", i ? " " : "", d->poc_st_curr_after[i]);
	printf("] l[");
	for (i = 0; i < d->num_poc_lt_curr; i++)
		printf("%s%u", i ? " " : "", d->poc_lt_curr[i]);
	printf("]");
}

/**
 * picture_complete() - The picture's record is complete: keep it for the output order, print it with --verbose.
 *
 * Closes the picture in the stream, then drains what it shows.
 */
static void picture_complete(void)
{
	unsigned number = record.number;
	if (record.no_rasl_output)
		cvs_count++;
	rp_stream_picture_done(&stream, &record);
	pictures++;
	if (number > last_number)
		last_number = number;
	if (number <= OUT_MAX)
	{
		our_poc[number] = record.poc;
		our_output[number] = record.output;
		our_cvs[number] = cvs_count;
	}
	if (verbose)
	{
		printf("picture %u: type %u POC %d%s%s slices %u dpb ", number, record.nal_type, record.poc, record.output ? "" : " (not output)",
		       record.corrupt ? " CORRUPT" : "", record.slice_count);
		print_dpb(&record.decode);
		printf("\n");
	}
	drain_output();
}

/**
 * note_refusal() - The stream was refused at a unit: keep where and why, and print it.
 * @type: the NAL unit type
 * @code: RP_PS_UNSUPPORTED or RP_PS_STREAM
 */
static void note_refusal(uint32_t type, enum rp_ps_result code)
{
	refused = stream.why != NULL ? stream.why : "(no reason)";
	refused_at = units;
	refused_type = type;
	refused_code = code;
	printf("unit %u (type %u) refused as %s: %s\n", units, type, code == RP_PS_UNSUPPORTED ? "unsupported" : "a stream error", refused);
}

/**
 * parameter_set() - Give a parameter set to the stream; print every SPS held with --verbose.
 * @type: the NAL unit type
 * @data: the unit's bytes, header included
 * @len: the length of data
 */
static void parameter_set(uint32_t type, const uint8_t *data, uint32_t len)
{
	enum rp_ps_result rc = rp_stream_parameter_set(&stream, type, data, len);
	if (rc != RP_PS_OK)
	{
		note_refusal(type, rc);
		return;
	}
	if (type != NAL_SPS_NUT || !verbose)
		return;
	/* every SPS held, with its conformance window: what make-conformance-refs.sh places the hash window by */
	for (unsigned id = 0; id < RP_SPS_MAX; id++)
	{
		const struct rp_sps *sps = &stream.sps[id];
		if (sps->valid)
			printf("sps %u: %ux%u depth %u window %u,%u,%u,%u\n", id, sps->width, sps->height, sps->bit_depth_luma, sps->conf_left,
			       sps->conf_top, sps->conf_right, sps->conf_bottom);
	}
}

/**
 * slice() - Give a slice segment to the stream and note a skipped or dropped picture or a refusal.
 * @type: the NAL unit type
 * @temporal_id: the unit's temporal id
 * @data: the unit's bytes, header included
 * @len: the length of data
 */
static void slice(uint32_t type, uint32_t temporal_id, const uint8_t *data, uint32_t len)
{
	enum rp_stream_result rc = rp_stream_slice(&stream, &record, data, len, type, temporal_id);
	if (rc == RP_STREAM_SKIP)
	{
		if (verbose)
			printf("unit %u: picture %u skipped: %s\n", units, stream.next_number, stream.why);
	}
	else if (rc == RP_STREAM_BAD)
		note_refusal(type, RP_PS_STREAM);
	else if (rc != RP_STREAM_OK)
	{
		printf("unit %u: picture %u dropped: %s\n", units, stream.next_number, stream.why);
		if (stream.in_picture || nal_starts_picture(data, len))
			dropped++; /* else a slice of a picture dropped already */
		rp_stream_picture_dropped(&stream);
	}
}

/**
 * unit() - Give one NAL unit to the stream as the session does.
 * @type: the NAL unit type
 * @temporal_id: the unit's temporal id
 * @data: the unit's bytes, header included
 * @len: the length of data
 */
static void unit(uint32_t type, uint32_t temporal_id, const uint8_t *data, uint32_t len)
{
	units++;
	if (refused != NULL)
		return;
	if (rp_stream_unit_ends_picture(&stream, type, data, len))
		picture_complete();
	if (nal_is_parameter_set(type))
		parameter_set(type, data, len);
	else if (type == NAL_EOS_NUT)
		rp_stream_end_of_sequence(&stream);
	else if (type == NAL_EOB_NUT)
	{
		rp_stream_end_of_bitstream(&stream);
		drain_output();
	}
	else if (nal_is_vcl(type))
		slice(type, temporal_id, data, len);
}

/**
 * read_file() - Read a whole file.
 * @path: the file
 * @len: out: its bytes
 *
 * Return: the bytes, to be freed; NULL when the file could not be read (the cause printed).
 */
static uint8_t *read_file(const char *path, uint32_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data = NULL;
	long size;
	if (f == NULL)
	{
		perror(path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size >= 0)
		data = malloc((size_t)size + 1);
	if (data != NULL && fread(data, 1, (size_t)size, f) != (size_t)size)
	{
		free(data);
		data = NULL;
	}
	fclose(f);
	if (data == NULL)
		fprintf(stderr, "%s: cannot read\n", path);
	else
		*len = (uint32_t)size;
	return data;
}

/**
 * run_stream() - Read an Annex B file and run its base-layer units through the stream.
 * @path: the file
 *
 * Return: 0 when read and run, -1 when the file could not be read.
 */
static int run_stream(const char *path)
{
	uint32_t len = 0, buf_len = 0;
	uint8_t *data = read_file(path, &len);
	uint8_t *buf = malloc(UNIT_MAX);
	struct rp_nal n;
	if (data == NULL || buf == NULL)
	{
		free(data);
		free(buf);
		return -1;
	}
	rp_nal_init(&n);
	rp_nal_packet(&n, data, len);
	for (;;)
	{
		enum rp_nal_event ev = rp_nal_next(&n);
		if (ev == RP_NAL_NONE)
			ev = rp_nal_finish(&n);
		if (ev == RP_NAL_NONE)
			break;
		if (ev == RP_NAL_START)
			buf_len = 0;
		if (ev != RP_NAL_END)
		{
			if (buf_len + n.len <= UNIT_MAX)
				memcpy(buf + buf_len, n.data, n.len);
			buf_len += n.len;
		}
		else if (buf_len <= UNIT_MAX && n.layer == NAL_BASE_LAYER) /* other layers ignored, as the session does */
			unit(n.type, n.temporal_id, buf, buf_len);
	}
	if (stream.in_picture)
		picture_complete();
	rp_stream_end_of_bitstream(&stream);
	drain_output();
	free(buf);
	free(data);
	return 0;
}

/**
 * out_of_order() - Whether a picture is shown after one it should come before.
 * @num: the picture
 * @prev: the picture shown before it
 *
 * Return: 1 when @num is of an earlier coded video sequence than @prev, or of the same with no higher POC.
 */
static int out_of_order(unsigned num, unsigned prev)
{
	if (prev > OUT_MAX)
		return 0;
	if (our_cvs[num] != our_cvs[prev])
		return our_cvs[num] < our_cvs[prev];
	return our_poc[num] <= our_poc[prev];
}

/**
 * check_output() - Check the output order.
 * @frames: the number of pictures FFmpeg showed, negative for no check
 *
 * No picture twice or shown although not for output, POCs ascending within a coded video sequence,
 * as many pictures as FFmpeg showed.
 *
 * Return: the number of differences found.
 */
static int check_output(long frames)
{
	static unsigned seen[OUT_MAX + 1];
	unsigned i;
	int diffs = 0;
	for (i = 0; i < out_count; i++)
	{
		unsigned num = out_numbers[i];
		int known = num <= OUT_MAX && num <= last_number;
		if (!known || seen[num]++ != 0 || !our_output[num])
		{
			printf("output %u: picture %u shown %s\n", i, num, known && our_output[num] ? "twice" : "although not for output");
			diffs++;
		}
		else if (i > 0 && out_of_order(num, out_numbers[i - 1]))
		{
			unsigned prev = out_numbers[i - 1];
			printf("output %u: picture %u POC %d (sequence %u) after picture %u POC %d (sequence %u)\n", i, num, our_poc[num],
			       our_cvs[num], prev, our_poc[prev], our_cvs[prev]);
			diffs++;
		}
	}
	if (frames >= 0 && (long)out_count != frames)
	{
		printf("output: %u pictures shown, FFmpeg showed %ld\n", out_count, frames);
		diffs++;
	}
	return diffs;
}

/* what the command line asks for */
struct options
{
	long frames;        /* --frames: the pictures FFmpeg showed; negative for no check */
	long dropped;       /* --dropped: the pictures the stream is made to lose; 0 unless given */
	int expect_refusal; /* --expect-refusal */
	const char *path;   /* the stream */
};

/**
 * parse_options() - The command line into the options.
 * @argc: the argument count
 * @argv: the arguments
 * @options: filled in
 *
 * Return: 0, or -1 for a command line that is not one of the two forms.
 */
static int parse_options(int argc, char **argv, struct options *options)
{
	int arg = 1;
	options->frames = -1;
	options->dropped = 0;
	options->expect_refusal = 0;
	for (; arg < argc && strncmp(argv[arg], "--", 2) == 0; arg++)
	{
		if (strcmp(argv[arg], "--frames") == 0 && arg + 1 < argc)
			options->frames = atol(argv[++arg]);
		else if (strcmp(argv[arg], "--dropped") == 0 && arg + 1 < argc)
			options->dropped = atol(argv[++arg]);
		else if (strcmp(argv[arg], "--verbose") == 0)
			verbose = 1;
		else if (strcmp(argv[arg], "--expect-refusal") == 0)
			options->expect_refusal = 1;
		else
			return -1;
	}
	if (argc - arg != 1)
		return -1;
	options->path = argv[arg];
	return 0;
}

/**
 * report_refusal() - Say how a stream that was to be refused fared.
 *
 * Return: 0 when it was refused as unsupported, 1 when it was not refused or refused as a stream error.
 */
static int report_refusal(void)
{
	if (refused == NULL)
	{
		printf("stream_check: not refused: %u pictures taken\n", pictures);
		return 1;
	}
	if (refused_code != RP_PS_UNSUPPORTED)
	{
		printf("stream_check: refused as a stream error, not as unsupported\n");
		return 1;
	}
	printf("stream_check: refused as planned at unit %u (type %u): %s\n", refused_at, refused_type, refused);
	return 0;
}

/**
 * main() - Parse the options, run the stream and report.
 * @argc: the argument count
 * @argv: the arguments
 *
 * Return: 0 when the order is right; 1 on a wrong order, other drops than --dropped says or a
 *         missing refusal; 2 on a usage or read error; 3 on a refusal not expected.
 */
int main(int argc, char **argv)
{
	struct options options;
	if (parse_options(argc, argv, &options) < 0)
	{
		fprintf(stderr, "usage: stream_check [--frames N] [--dropped N] [--verbose] file.h265 | stream_check --expect-refusal file.h265\n");
		return 2;
	}
	rp_stream_init(&stream);
	memset(&record, 0, sizeof(record));
	record.slices = calloc(RP_SLICES_MAX, sizeof(*record.slices));
	record.slice_cap = RP_SLICES_MAX;
	if (record.slices == NULL || run_stream(options.path) < 0)
		return 2;
	if (options.expect_refusal)
		return report_refusal();
	if (refused != NULL)
	{
		printf("stream_check: refused at unit %u (type %u): %s\n", refused_at, refused_type, refused);
		return 3;
	}
	int order = check_output(options.frames);
	printf("stream_check: %u pictures, %u dropped, %u shown, order %s\n", pictures, dropped, out_count, order ? "wrong" : "right");
	return dropped == (unsigned long)options.dropped && order == 0 && pictures != 0 ? 0 : 1;
}
