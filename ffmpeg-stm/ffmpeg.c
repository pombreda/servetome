/*
 *  ffmpeg-servetome.c
 *  ffmpeg_servetome
 *
 *  Created by Matt Gallagher on 2010/01/13.
 *  Copyright 2010 Matt Gallagher. All rights reserved.
 *
 */

#define GOP_LENGTH 2.0
#define TRANSITION_REPEAT 0.8
#define REPEAT_COMPENSATION_RANGE 40.0

/* needed for usleep() */
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE

#include "config.h"
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavcodec/opt.h"
#include "libavcodec/audioconvert.h"
#include "libavcodec/colorspace.h"
#include "libavutil/fifo.h"
#include "libavutil/avstring.h"
#include "libavformat/os_support.h"
#include "libavfilter/avfilter.h"
#include "libavfilter/avfiltergraph.h"
#include "libavfilter/graphparser.h"
#include "libavfilter/vsrc_buffer.h"
#include "libavfilter/vf_inlineass.h"


#if HAVE_SYS_RESOURCE_H
#include <sys/types.h>
#include <sys/resource.h>
#elif HAVE_GETPROCESSTIMES
#include <windows.h>
#endif

#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#if HAVE_TERMIOS_H
#include <fcntl.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#elif HAVE_CONIO_H
#include <conio.h>
#endif

#undef time //needed because HAVE_AV_CONFIG_H is defined on top
#include <time.h>
#include <pthread.h>

#undef NDEBUG
#include <assert.h>

#undef exit

#define LINUX
#define VERBOSE 0
const char *program_name;
const char *program_birth_year;

#ifdef WINDOWS
#include <asprintf.h>
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifdef LINUX
#include <asprintf.h>
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#include <fontconfig/fontconfig.h>

char *executable_path;
char *subtitle_path;

/* Global logging */
static int verbose = VERBOSE;

static int64_t file_size = 0;
static int64_t video_size = 0;
static int64_t audio_size = 0;
static int64_t extra_size = 0;
static int nb_frames_dup = 0;
static int nb_frames_drop = 0;
static int64_t timer_start;

/* select an input stream for an output stream */
typedef struct AVStreamMap {
    int file_index;
    int stream_index;
    int sync_file_index;
    int sync_stream_index;
} AVStreamMap;

/** select an input file for an output file */
typedef struct AVMetaDataMap {
    int out_file;
    int in_file;
} AVMetaDataMap;

typedef struct STMVideoSettings {

	int64_t input_start_time;
	char *input_file_name;

	AVRational source_video_frame_rate;
	float source_video_aspect_ratio;
	float source_audio_channel_layout;

	char *output_file_format;
	const char *output_file_base;
	char *output_file_path;
	float output_dts_delta_threshold;
	int64_t output_duration;
	double output_segment_length;
	int64_t output_segment_index;
	int64_t output_initial_segment;
	int output_single_frame_only;
	int64_t last_output_pts;
	int64_t last_mux_pts;
	int output_padding;
	
	int skip_mode_enabled;
	int album_art_mode;

	char *video_codec_name;
	AVRational video_frame_rate;
	int video_frame_width;
	int video_frame_height;
	int source_frame_width;
	int source_frame_height;
	
	int video_frame_refs;
	int video_crf;
	int video_level;
	int video_me_range;
	char *video_me_method;
	int video_sc_threshold;
	int video_qdiff;
	int video_qmin;
	int video_qmax;
	float video_qcomp;
	int video_g;
	float video_subq;
	int video_bufsize;
	int video_minrate;
	int video_maxrate;
	char *video_flags;
	char *video_flags2;
	char *video_cmp;
	char *video_partitions;
	
	char *video_filters;

	char *audio_codec_name;
	int audio_sample_rate;
	int audio_channels;
	float audio_drift_threshold;
	int audio_volume;
	int audio_bitrate;
	char *audio_stream_specifier;

	char *subtitle_stream_specifier;
	int subtitle_stream_index;
} STMTranscodeSettings;

typedef struct STMTranscodeContext {
	STMTranscodeSettings settings;

	int64_t *input_files_ts_offset;

	AVFormatContext **input_files;
	size_t num_input_files;
	size_t input_files_capacity;

	AVFormatContext **output_files;
	size_t num_output_files;
	size_t output_files_capacity;

	AVCodec **input_codecs;
	size_t num_input_codecs;
	size_t input_codecs_capacity;

	AVCodec **output_codecs;
	size_t num_output_codecs;
	size_t output_codecs_capacity;

	AVStreamMap *stream_maps;
	size_t num_stream_maps;
	size_t stream_maps_capacity;

	AVMetaDataMap *meta_data_maps;
	size_t num_meta_data_maps;
	size_t meta_data_maps_capacity;
	
	AVMetadataTag *metadata;
	int metadata_count;

	char *video_filters;
	AVFilterGraph *filt_graph_all;
	AVFilterContext *inlineass_context;
} STMTranscodeContext;

//static unsigned int sws_flags = SWS_BICUBIC;

struct AVInputStream;

typedef struct AVOutputStream {
    int file_index;          /* file index */
    int index;               /* stream index in the output file */
    int source_index;        /* AVInputStream index */
    AVStream *st;            /* stream in the output file */
    int encoding_needed;     /* true if encoding needed for this stream */
    int frame_number;
    /* input pts and corresponding output pts
       for A/V sync */
    //double sync_ipts;        /* dts from the AVPacket of the demuxer in second units */
    struct AVInputStream *sync_ist; /* input stream to sync against */
    int64_t sync_opts;       /* output frame counter, could be changed to some true timestamp */ //FIXME look at frame_number
    /* video only */
    int video_resample;
    AVFrame pict_tmp;      /* temporary image for resampling */
    struct SwsContext *img_resample_ctx; /* for image resampling */
    int resample_height;
    int resample_width;
    int resample_pix_fmt;

    /* full frame size of first frame */
    int original_height;
    int original_width;

    /* audio only */
    int audio_resample;
    ReSampleContext *resample; /* for audio resampling */
    int reformat_pair;
    AVAudioConvert *reformat_ctx;
    AVFifoBuffer *fifo;     /* for compression: one audio fifo per codec */
} AVOutputStream;

typedef struct AVInputStream {
    int file_index;
    int index;
    AVStream *st;
    int discard;             /* true if stream data should be discarded */
    int decoding_needed;     /* true if the packets must be decoded in 'raw_fifo' */
    int64_t sample_index;      /* current sample */

    int64_t       start;     /* time when read started */
    int64_t       next_pts;  /* synthetic pts for cases where pkt.pts
                                is not defined */
    int64_t       pts;       /* current pts */
    int is_start;            /* is 1 at the start and after a discontinuity */
    AVFilterContext *out_video_filter;
    AVFilterContext *input_video_filter;
    AVFrame *filter_frame;
    int has_filter_frame;
    AVFilterPicRef *picref;
} AVInputStream;

typedef struct AVInputFile {
    int eof_reached;      /* true if eof reached */
    int ist_index;        /* index of first stream in ist_table */
    int buffer_size;      /* current total buffer size */
    int nb_streams;       /* nb streams we are aware of */
} AVInputFile;

typedef struct {
    int pix_fmt;
} FilterOutPriv;

void AddNewInputCodec(STMTranscodeContext *context, AVCodec *codec);


static int64_t SegmentEnd(STMTranscodeSettings *settings)
{
	int64_t segment_end = 
		AV_TIME_BASE * settings->output_segment_length *
		(settings->output_segment_index + 1);
	return segment_end;
}

static int64_t BaseTime(STMTranscodeSettings *settings, int64_t stream_pts, AVRational stream_time_base)
{
	int64_t current_pts =
		av_rescale_q(stream_pts, stream_time_base, AV_TIME_BASE_Q) +
			settings->input_start_time;
	return current_pts;
}

static void SignalHandler(int signal)
{
	//
	// A signal handler which catches exceptions only to quit might seem
	// pointless but it stops error dialogs about uncaught exceptions on
	// Windows.
	//
	printf("Caught a signal!! (%d)\n",signal);
	exit(1);
}

#ifndef WINDOWS
#ifndef LINUX
void* WatchForParentTermination (void* arg) {	
	pid_t ppid = getppid ();	// get parent pid 

	int kq = kqueue (); 
	if (kq != -1) { 
		struct kevent procEvent;	// wait for parent to exit 
		EV_SET (&procEvent,	 // kevent 
			ppid,	 // ident 
			EVFILT_PROC,	// filter 
			EV_ADD,	 // flags 
			NOTE_EXIT,	 // fflags 
			0,	 // data 
			0);	 // udata 
		kevent (kq, &procEvent, 1, &procEvent, 1, 0); 
	}
	exit (0);	
	return 0; 
}
#endif
#endif

static int output_init(AVFilterContext *ctx, const char *args, void *opaque)
{
    FilterOutPriv *priv = ctx->priv;

    if(!opaque) return -1;

    priv->pix_fmt = *((int *)opaque);

    return 0;
}

static void output_end_frame(AVFilterLink *link)
{
}

static int output_query_formats(AVFilterContext *ctx)
{
    FilterOutPriv *priv = ctx->priv;
    enum PixelFormat pix_fmts[] = { priv->pix_fmt, PIX_FMT_NONE };

    avfilter_set_common_formats(ctx, avfilter_make_format_list(pix_fmts));
    return 0;
}

static int get_filtered_video_pic(AVFilterContext *ctx,
                                  AVFilterPicRef **picref, AVFrame *pic2,
                                  uint64_t *pts)
{
    AVFilterPicRef *pic;

    if(avfilter_request_frame(ctx->inputs[0]))
        return -1;
    if(!(pic = ctx->inputs[0]->cur_pic))
        return -1;
    *picref = pic;
    ctx->inputs[0]->cur_pic = NULL;

    *pts          = pic->pts;

    memcpy(pic2->data,     pic->data,     sizeof(pic->data));
    memcpy(pic2->linesize, pic->linesize, sizeof(pic->linesize));

    return 1;
}

static AVFilter output_filter =
{
    .name      = "ffmpeg_output",

    .priv_size = sizeof(FilterOutPriv),
    .init      = output_init,

    .query_formats = output_query_formats,

    .inputs    = (AVFilterPad[]) {{ .name          = "default",
                                    .type          = CODEC_TYPE_VIDEO,
                                    .end_frame     = output_end_frame,
                                    .min_perms     = AV_PERM_READ, },
                                  { .name = NULL }},
    .outputs   = (AVFilterPad[]) {{ .name = NULL }},
};

static int configure_filters(STMTranscodeContext *context,
	AVInputStream *ist, AVOutputStream *ost)
{
    AVFilterContext *curr_filter;
    /** filter graph containing all filters including input & output */
    AVCodecContext *codec = ost->st->codec;
    AVCodecContext *icodec = ist->st->codec;
    char args[255];

    context->filt_graph_all = av_mallocz(sizeof(AVFilterGraph));

    if(!(ist->input_video_filter = avfilter_open(avfilter_get_by_name("buffer"), "src")))
        return -1;
    if(!(ist->out_video_filter = avfilter_open(&output_filter, "out")))
        return -1;

    snprintf(args, 255, "%d:%d:%d", ist->st->codec->width,
             ist->st->codec->height, ist->st->codec->pix_fmt);
    if(avfilter_init_filter(ist->input_video_filter, args, NULL))
        return -1;
    if(avfilter_init_filter(ist->out_video_filter, NULL, &codec->pix_fmt))
        return -1;

    /* add input and output filters to the overall graph */
    avfilter_graph_add_filter(context->filt_graph_all, ist->input_video_filter);
    avfilter_graph_add_filter(context->filt_graph_all, ist->out_video_filter);

    curr_filter = ist->input_video_filter;

    if((codec->width != icodec->width) || (codec->height != icodec->height)) {
        char crop_args[255];
        AVFilterContext *filt_scale;
        snprintf(crop_args, 255, "%d:%d:sws_flags=%d",
                 codec->width,
                 codec->height,
                 0);
        filt_scale = avfilter_open(avfilter_get_by_name("scale"), NULL);
        if (!filt_scale)
            return -1;
        if (avfilter_init_filter(filt_scale, crop_args, NULL))
            return -1;
        if (avfilter_link(curr_filter, 0, filt_scale, 0))
            return -1;
        curr_filter = filt_scale;
        avfilter_graph_add_filter(context->filt_graph_all, curr_filter);
    }

    if(context->settings.video_filters) {
        AVFilterInOut *outputs = av_malloc(sizeof(AVFilterInOut));
        AVFilterInOut *inputs  = av_malloc(sizeof(AVFilterInOut));

        outputs->name    = av_strdup("in");
        outputs->filter  = curr_filter;
        outputs->pad_idx = 0;
        outputs->next    = NULL;

        inputs->name    = av_strdup("out");
        inputs->filter  = ist->out_video_filter;
        inputs->pad_idx = 0;
        inputs->next    = NULL;

        if (avfilter_graph_parse(context->filt_graph_all, context->settings.video_filters, inputs, outputs, NULL) < 0)
            return -1;
    } else {
        if(avfilter_link(curr_filter, 0, ist->out_video_filter, 0) < 0)
            return -1;
    }

    {
        char scale_sws_opts[128];
        snprintf(scale_sws_opts, sizeof(scale_sws_opts), "sws_flags=%d", 0);
        context->filt_graph_all->scale_sws_opts = av_strdup(scale_sws_opts);
    }

    /* configure all the filter links */
    if(avfilter_graph_check_validity(context->filt_graph_all, NULL))
        return -1;
    if(avfilter_graph_config_formats(context->filt_graph_all, NULL))
        return -1;
    if(avfilter_graph_config_links(context->filt_graph_all, NULL))
        return -1;
	
	for (int i = 0; i < context->filt_graph_all->filter_count; i++)
	{
		if (strcmp(context->filt_graph_all->filters[i]->filter->name, "inlineass") == 0)
		{
			context->inlineass_context = context->filt_graph_all->filters[i];
			vf_inlineass_set_aspect_ratio(context->inlineass_context,
				av_q2d(ost->st->codec->sample_aspect_ratio));
		}
	}

    codec->width = ist->out_video_filter->inputs[0]->w;
    codec->height = ist->out_video_filter->inputs[0]->h;

    return 0;
}

char *inmemory_buffer;
int64_t inmemory_buffer_size;
URLProtocol inmemory_protocol;

static int in_memory_url_open(URLContext *h, const char *filename, int flags)
{
	h->priv_data = 0;
	return 0;
}
static int in_memory_url_read(URLContext *h, unsigned char *buf, int size)
{
	if ((int)h->priv_data + size > inmemory_buffer_size)
	{
		size = inmemory_buffer_size - (int)h->priv_data;
		if (size == 0)
		{
			return 0;
		}
	}
	memcpy(buf, &inmemory_buffer[(int)h->priv_data], size);
	h->priv_data = (void *)((int)h->priv_data + size);
	return size;
}
static int in_memory_url_write(URLContext *h, unsigned char *buf, int size)
{
	if ((int)h->priv_data + size > inmemory_buffer_size)
	{
		size = inmemory_buffer_size - (int)h->priv_data;
	}
	memcpy(&inmemory_buffer[(int)h->priv_data], buf, size);
	h->priv_data = (void *)((int)h->priv_data + size);
	return size;
}
static int64_t in_memory_url_seek(URLContext *h, int64_t pos, int whence)
{
	if (whence == AVSEEK_SIZE)
	{
		return inmemory_buffer_size;
	}
	else if (whence == SEEK_END)
	{
		h->priv_data = (void *)((int)inmemory_buffer_size);
		return inmemory_buffer_size;
	}
	else if (whence == SEEK_CUR)
	{
		h->priv_data = (void *)((int)inmemory_buffer_size);
		return (int)h->priv_data;
	}
	
	h->priv_data = (void *)((int)pos);
	return 0;
}

static int in_memory_url_close(URLContext *h)
{
	return 0;
}

static int in_memory_url_read_pause(URLContext *h, int pause)
{
	return 0;
}

static int64_t in_memory_url_read_seek(URLContext *h, int stream_index, int64_t timestamp, int flags)
{
	return 0;
}

static int in_memory_url_get_file_handle(URLContext *h)
{
	return (int)inmemory_buffer;
}

static void register_inmemory_protocol()
{
	inmemory_protocol.name = "inmemory";
	inmemory_protocol.next = NULL;
	inmemory_protocol.url_open = in_memory_url_open;
	inmemory_protocol.url_read = in_memory_url_read;
	inmemory_protocol.url_write = in_memory_url_write;
	inmemory_protocol.url_seek = in_memory_url_seek;
	inmemory_protocol.url_close = in_memory_url_close;
	inmemory_protocol.url_read_pause = in_memory_url_read_pause;
	inmemory_protocol.url_read_seek = in_memory_url_read_seek;
	inmemory_protocol.url_get_file_handle = in_memory_url_get_file_handle;
	
	av_register_protocol(&inmemory_protocol);
}

static int threadCount()
{
	static int threadCount = 0;
	
	if (threadCount == 0)
	{
#ifdef WINDOWS
		SYSTEM_INFO sysInfo;
		GetSystemInfo(&sysInfo);
		threadCount = sysInfo.dwNumberOfProcessors;
#else
#ifdef LINUX
		//threadCount=1;
        threadCount = sysconf( _SC_NPROCESSORS_ONLN );
#else
		int mib[2];
		size_t len;

		mib[0] = CTL_HW;
		mib[1] = HW_NCPU;
		len = sizeof(threadCount);

		sysctl((int *)mib, (u_int)2, &threadCount, &len, NULL, 0);
#endif
#endif
		threadCount = MAX(2, threadCount);

		if(verbose > 0)
			fprintf(stderr, "Thread count: %d\n", threadCount);
	}
	
	return threadCount;
}

static void realloc_storage(void **pointer, size_t unit_size, size_t *capacity, size_t count)
{
	if (*pointer && count <= *capacity)
	{
		return;
	}
	
	if (count + 4 > *capacity)
	{
		*capacity = count + 4;
	}
	
	if (!(*pointer))
	{
		*pointer = av_malloc(unit_size * (*capacity));
	}
	else
	{
		*pointer = av_realloc(*pointer, unit_size * (*capacity));
	}
}

void AddNewInputCodec(STMTranscodeContext *context, AVCodec *codec)
{
//    printf("Hello %x %x\n",context,codec);
	context->num_input_codecs++;
//    printf("Hello %x %x\n",context,codec);
	realloc_storage((void **)&context->input_codecs, sizeof(AVCodec *), &context->input_codecs_capacity, context->num_input_codecs);
//    printf("Hello %x %x\n",context,codec);
    fprintf(stderr,"Without this ffmpeg segfaults, compiler issue\n");
	context->input_codecs[context->num_input_codecs - 1] = codec;
//    printf("Hello %x %x\n",context,codec);
}
static void AddNewOutputCodec(STMTranscodeContext *context, AVCodec *codec)
{
	context->num_output_codecs++;
	realloc_storage((void **)&context->output_codecs, sizeof(AVCodec *), &context->output_codecs_capacity, context->num_output_codecs);
	context->output_codecs[context->num_output_codecs - 1] = codec;
}
static void AddNewInputFile(STMTranscodeContext *context, AVFormatContext *file, int64_t ts_offset)
{
	context->num_input_files++;
	realloc_storage((void **)&context->input_files, sizeof(AVFormatContext *), &context->input_files_capacity, context->num_input_files);
	realloc_storage((void **)&context->input_files_ts_offset, sizeof(int64_t), &context->input_files_capacity, context->num_input_files);
	context->input_files[context->num_input_files - 1] = file;
	context->input_files_ts_offset[context->num_input_files - 1] = ts_offset;
}
static void AddNewOutputFile(STMTranscodeContext *context, AVFormatContext *file)
{
	context->num_output_files++;
	realloc_storage((void **)&context->output_files, sizeof(AVFormatContext *), &context->output_files_capacity, context->num_output_files);
	context->output_files[context->num_output_files - 1] = file;
}
static void AddNewStreamMap(STMTranscodeContext *context, AVStreamMap map)
{
	context->num_stream_maps++;
	realloc_storage((void **)&context->stream_maps, sizeof(AVStreamMap), &context->stream_maps_capacity, context->num_stream_maps);
	context->stream_maps[context->num_stream_maps - 1] = map;
}
static void AddNewMetaDataMap(STMTranscodeContext *context, AVMetaDataMap map)
{
	context->num_meta_data_maps++;
	realloc_storage((void **)&context->meta_data_maps, sizeof(AVMetaDataMap), &context->meta_data_maps_capacity, context->num_meta_data_maps);
	context->meta_data_maps[context->num_meta_data_maps - 1] = map;
}

static size_t FilenameSuffixLength()
{
	// Length of blah excluding %s when %d is expanded... "%s-%05d.ts"
	return 9;
}

static void UpdateFilePathForSegment(STMTranscodeSettings *settings)
{
	snprintf(settings->output_file_path,
		strlen(settings->output_file_base) + FilenameSuffixLength() + 1,
		"%s-%05lld.ts",
		settings->output_file_base,
		settings->output_segment_index);
}

static double
get_sync_ipts(const AVOutputStream *ost, STMTranscodeSettings *settings)
{
    const AVInputStream *ist = ost->sync_ist;

	double sync_ipts = (double)(ist->pts) / AV_TIME_BASE;

	if (settings->output_initial_segment != 0)
	{
		if (sync_ipts < REPEAT_COMPENSATION_RANGE + TRANSITION_REPEAT)
		{
			#define HALF_SIGMOID_WIDTH 1.5
			#define CLAMP 0.968
			
			double t = 2 * HALF_SIGMOID_WIDTH * sync_ipts / (REPEAT_COMPENSATION_RANGE + TRANSITION_REPEAT) - HALF_SIGMOID_WIDTH;
			double sigmoid = erf(t);
			
			sync_ipts =
				sync_ipts -
				TRANSITION_REPEAT * 0.5 * (sigmoid + CLAMP) * (1.0 / CLAMP);
		}
		else
		{
			sync_ipts -= TRANSITION_REPEAT;
		}
	}
	return sync_ipts;
}

static int process_segment_change(STMTranscodeSettings *settings, AVFormatContext *s, AVPacket *pkt)
{
	if (settings->output_segment_length == 0 || pkt->pts == AV_NOPTS_VALUE)
	{
		return 0;
	}
	
	int64_t end_ts = settings->output_duration;
	int64_t segment_end = SegmentEnd(settings);
	int64_t current_pts = BaseTime(settings,  pkt->pts, s->streams[pkt->stream_index]->time_base);

	settings->last_output_pts = current_pts;
	
//	fprintf(stderr, "outputting pts %lld for stream %d\n", current_pts, pkt->stream_index);

	if (current_pts >= segment_end &&
		current_pts <= end_ts
		)
	{
		put_flush_packet(s->pb);
		
		file_size += url_ftell(s->pb);
		
		url_fclose(s->pb);
		
		fprintf(stdout, "Wrote segment %lld\n", settings->output_segment_index);
		fflush(stdout);

#if !defined(VERBOSE) || VERBOSE > 0
		if (getenv("NoAcknowledgements") == NULL)
		{
#endif

		#define ACK_LENGTH 3
		char ackRead[ACK_LENGTH];
		size_t read = fread(ackRead, ACK_LENGTH, 1, stdin);
		if (read != 1 || strncmp(ackRead, "ack", ACK_LENGTH) != 0)
		{
			fprintf(stderr, "Acknowledgment failure: %ld, %.3s\n", read, ackRead);
			exit(0);
		}
		
#if !defined(VERBOSE) || VERBOSE > 0
		}
#endif
		settings->output_segment_index++;
		UpdateFilePathForSegment(settings);
		
		if (url_fopen(&s->pb, settings->output_file_path, URL_WRONLY) < 0)
		{
			fprintf(stderr, "Couldn't open '%s'\n", settings->output_file_path);
			s->pb = NULL;
			return -1;
		}
	}
	return 0;
}

static int write_frame(STMTranscodeSettings *settings,
	AVFormatContext *s, AVPacket *pkt, AVCodecContext *avctx)
{
    AVStream *st= s->streams[ pkt->stream_index];

    //FIXME/XXX/HACK drop zero sized packets
    if(st->codec->codec_type == CODEC_TYPE_AUDIO && pkt->size==0)
        return 0;

//av_log(NULL, AV_LOG_DEBUG, "av_interleaved_write_frame %d %"PRId64" %"PRId64"\n", pkt->size, pkt->dts, pkt->pts);
    if(compute_pkt_fields2(s, st, pkt) < 0 && !(s->oformat->flags & AVFMT_NOTIMESTAMPS))
        return -1;
	
	// Drop subtitle packets.
	if (st->codec->codec_type == CODEC_TYPE_SUBTITLE)
	{
		return 0;
	}

    if(pkt->dts == AV_NOPTS_VALUE && !(s->oformat->flags & AVFMT_NOTIMESTAMPS))
        return -1;

	int64_t current_pts = BaseTime(settings, pkt->pts, s->streams[pkt->stream_index]->time_base);

    for(;;){
		int flush =
			current_pts > settings->last_mux_pts + settings->output_segment_length * AV_TIME_BASE ||
			settings->output_padding;

        AVPacket opkt;
        int ret= av_interleave_packet_per_dts(s, &opkt, pkt, flush);
        if(ret<=0) //FIXME cleanup needed for ret<0 ?
            return ret;

		if (process_segment_change(settings, s, &opkt) != 0)
		{
			return -1;
		}

        ret= s->oformat->write_packet(s, &opkt);

		settings->last_mux_pts =
			av_rescale_q(opkt.pts, s->streams[opkt.stream_index]->time_base, AV_TIME_BASE_Q);

//		if (opkt.stream_index == 0 && opkt.flags & PKT_FLAG_KEY)
//		{
//			fprintf(stderr,
//				"KEEEEEY packet pkt_pts:%ld\n",
//				lround((settings->last_mux_pts * settings->video_frame_rate.num) / (double)AV_TIME_BASE));
//		}
//		else if (opkt.stream_index == 0)
//		{
//			fprintf(stderr, "Writing packet pkt_pts:%ld\n",
//				lround((settings->last_mux_pts * settings->video_frame_rate.num) / (double)AV_TIME_BASE));
//		}
//		else
//		{
//			fprintf(stderr, "Writing packet pkt_pts:%"PRId64"\n", settings->last_mux_pts);
//		}

        av_free_packet(&opkt);
        pkt= NULL;

        if(ret<0)
            return ret;
        if(url_ferror(s->pb))
            return url_ferror(s->pb);
    }
}

#define MAKE_SFMT_PAIR(a,b) ((a)+SAMPLE_FMT_NB*(b))
#define MAX_AUDIO_PACKET_SIZE (1 * 1024 * 1024)

static int do_audio_out(STMTranscodeSettings *settings,
						 AVFormatContext *s,
                         AVOutputStream *ost,
                         AVInputStream *ist,
                         unsigned char *buf, int size)
{
	static uint8_t *audio_buf;
	static uint8_t *audio_out;
    int64_t allocated_for_size= size;
	static unsigned int allocated_audio_out_size, allocated_audio_buf_size;
    uint8_t *buftmp;
    int64_t audio_out_size, audio_buf_size;

    int size_out, frame_bytes, ret;
    AVCodecContext *enc= ost->st->codec;
    AVCodecContext *dec= ist->st->codec;
    int osize= av_get_bits_per_sample_format(enc->sample_fmt)/8;
    int isize= av_get_bits_per_sample_format(dec->sample_fmt)/8;
    const int coded_bps = av_get_bits_per_sample(enc->codec->id);

need_realloc:
    audio_buf_size= (allocated_for_size + isize*dec->channels - 1) / (isize*dec->channels);
    audio_buf_size= (audio_buf_size*enc->sample_rate + dec->sample_rate) / dec->sample_rate;
    audio_buf_size= audio_buf_size*2 + 10000; //safety factors for the deprecated resampling API
    audio_buf_size*= osize*enc->channels;

    audio_out_size= FFMAX(audio_buf_size, enc->frame_size * osize * enc->channels);
    if(coded_bps > 8*osize)
        audio_out_size= audio_out_size * coded_bps / (8*osize);
    audio_out_size += FF_MIN_BUFFER_SIZE;

    if(audio_out_size > INT_MAX || audio_buf_size > INT_MAX){
        fprintf(stderr, "Buffer sizes too large\n");
        return -1;
    }

    av_fast_malloc(&audio_buf, &allocated_audio_buf_size, audio_buf_size);
    av_fast_malloc(&audio_out, &allocated_audio_out_size, audio_out_size);
    if (!audio_buf || !audio_out){
        fprintf(stderr, "Out of memory in do_audio_out\n");
        return -1;
    }

    if (enc->channels != dec->channels)
        ost->audio_resample = 1;

    if (ost->audio_resample && !ost->resample) {
        if (dec->sample_fmt != SAMPLE_FMT_S16 && verbose > 0)
            fprintf(stderr, "Warning, using s16 intermediate sample format for resampling\n");
        ost->resample = av_audio_resample_init(enc->channels,    dec->channels,
                                               enc->sample_rate, dec->sample_rate,
                                               enc->sample_fmt,  dec->sample_fmt,
                                               16, 10, 0, 0.8);
        if (!ost->resample) {
            fprintf(stderr, "Can not resample %d channels @ %d Hz to %d channels @ %d Hz\n",
                    dec->channels, dec->sample_rate,
                    enc->channels, enc->sample_rate);
            return -1;
        }
    }

    if (!ost->audio_resample && dec->sample_fmt!=enc->sample_fmt &&
        MAKE_SFMT_PAIR(enc->sample_fmt,dec->sample_fmt)!=ost->reformat_pair) {
        if (ost->reformat_ctx)
            av_audio_convert_free(ost->reformat_ctx);
        ost->reformat_ctx = av_audio_convert_alloc(enc->sample_fmt, 1,
                                                   dec->sample_fmt, 1, NULL, 0);
        if (!ost->reformat_ctx) {
            fprintf(stderr, "Cannot convert %s sample format to %s sample format\n",
                avcodec_get_sample_fmt_name(dec->sample_fmt),
                avcodec_get_sample_fmt_name(enc->sample_fmt));
            return -1;
        }
        ost->reformat_pair=MAKE_SFMT_PAIR(enc->sample_fmt,dec->sample_fmt);
    }

	if (!settings->album_art_mode)
	{
		double delta = get_sync_ipts(ost, settings) * enc->sample_rate - ost->sync_opts
				- av_fifo_size(ost->fifo)/(ost->st->codec->channels * 2);
		double idelta= delta*ist->st->codec->sample_rate / enc->sample_rate;
		int byte_delta= ((int)idelta)*2*ist->st->codec->channels;

		//FIXME resample delay
		if(fabs(delta) > 0.001 * enc->sample_rate){
			if(ist->is_start || fabs(delta) > settings->audio_drift_threshold*enc->sample_rate){
				if(byte_delta < 0){
					byte_delta= FFMAX(byte_delta, -size);
					size += byte_delta;
					buf  -= byte_delta;
					if(verbose > 2)
						fprintf(stderr, "discarding %d audio samples\n", (int)-delta);
					if(!size)
						return 0;
					ist->is_start=0;
				}else if(byte_delta < MAX_AUDIO_PACKET_SIZE) {
					static uint8_t *input_tmp= NULL;
					input_tmp= av_realloc(input_tmp, byte_delta + size);

					if(byte_delta > allocated_for_size - size){
						allocated_for_size= byte_delta + (int64_t)size;
						goto need_realloc;
					}
					ist->is_start=0;

					memset(input_tmp, 0, byte_delta);
					memcpy(input_tmp + byte_delta, buf, size);
					buf= input_tmp;
					size += byte_delta;
					if(verbose > 2)
						fprintf(stderr, "adding %d audio samples of silence\n", (int)delta);
				}
				else
				{
					ost->sync_opts= lrintf(get_sync_ipts(ost, settings) * enc->sample_rate)
									- av_fifo_size(ost->fifo)/(ost->st->codec->channels * 2); //FIXME wrong
					ost->st->pts.val =
						av_rescale_q(ost->sync_ist->pts, AV_TIME_BASE_Q, ost->st->time_base);
				}
			} else {
				int comp= av_clip(delta, -settings->audio_drift_threshold * enc->sample_rate, settings->audio_drift_threshold * enc->sample_rate);
				assert(ost->audio_resample);
				if(verbose > 2)
					fprintf(stderr, "compensating audio timestamp drift:%f compensation:%d in:%d\n", delta, comp, enc->sample_rate);
				av_resample_compensate(*(struct AVResampleContext**)ost->resample, comp, enc->sample_rate);
			}
		}
    }else
        ost->sync_opts= lrintf(get_sync_ipts(ost, settings) * enc->sample_rate)
                        - av_fifo_size(ost->fifo)/(ost->st->codec->channels * 2); //FIXME wrong

    if (ost->audio_resample) {
        buftmp = audio_buf;
        size_out = audio_resample(ost->resample,
                                  (short *)buftmp, (short *)buf,
                                  size / (ist->st->codec->channels * isize));
        size_out = size_out * enc->channels * osize;
    } else {
        buftmp = buf;
        size_out = size;
    }

    if (!ost->audio_resample && dec->sample_fmt!=enc->sample_fmt) {
        const void *ibuf[6]= {buftmp};
        void *obuf[6]= {audio_buf};
        int istride[6]= {isize};
        int ostride[6]= {osize};
        int len= size_out/istride[0];
        if (av_audio_convert(ost->reformat_ctx, obuf, ostride, ibuf, istride, len)<0) {
            printf("av_audio_convert() failed\n");
            return 0;
        }
        buftmp = audio_buf;
        size_out = len*osize;
    }

    /* now encode as many frames as possible */
    if (enc->frame_size > 1) {
        /* output resampled raw samples */
        if (av_fifo_realloc2(ost->fifo, av_fifo_size(ost->fifo) + size_out) < 0) {
            fprintf(stderr, "av_fifo_realloc2() failed\n");
            return -1;
        }
        av_fifo_generic_write(ost->fifo, buftmp, size_out, NULL);

        frame_bytes = enc->frame_size * osize * enc->channels;

        while (av_fifo_size(ost->fifo) >= frame_bytes) {
            AVPacket pkt;
            av_init_packet(&pkt);

            av_fifo_generic_read(ost->fifo, audio_buf, frame_bytes, NULL);

            //FIXME pass ost->sync_opts as AVFrame.pts in avcodec_encode_audio()

            ret = avcodec_encode_audio(enc, audio_out, audio_out_size,
                                       (short *)audio_buf);
            if (ret < 0) {
                fprintf(stderr, "Audio encoding failed\n");
                return ret;
            }
            audio_size += ret;
            pkt.stream_index= ost->index;
            pkt.data= audio_out;
            pkt.size= ret;
            if(enc->coded_frame && enc->coded_frame->pts != AV_NOPTS_VALUE)
                pkt.pts= av_rescale_q(enc->coded_frame->pts, enc->time_base, ost->st->time_base);
            pkt.flags |= PKT_FLAG_KEY;
            
			if (write_frame(settings, s, &pkt, ost->st->codec))
			{
				return -1;
			}

			ost->sync_opts += enc->frame_size;
        }
    } else {
        AVPacket pkt;
        av_init_packet(&pkt);

		ost->sync_opts += size_out / (osize * enc->channels);

        /* output a pcm frame */
        /* determine the size of the coded buffer */
        size_out /= osize;
        if (coded_bps)
            size_out = size_out*coded_bps/8;

        if(size_out > audio_out_size){
            fprintf(stderr, "Internal error, buffer size too small\n");
            return -1;
        }

        //FIXME pass ost->sync_opts as AVFrame.pts in avcodec_encode_audio()
        ret = avcodec_encode_audio(enc, audio_out, size_out,
                                   (short *)buftmp);
        if (ret < 0) {
            fprintf(stderr, "Audio encoding failed\n");
            return ret;
        }
        audio_size += ret;
        pkt.stream_index= ost->index;
        pkt.data= audio_out;
        pkt.size= ret;
        if(enc->coded_frame && enc->coded_frame->pts != AV_NOPTS_VALUE)
            pkt.pts= av_rescale_q(enc->coded_frame->pts, enc->time_base, ost->st->time_base);
        pkt.flags |= PKT_FLAG_KEY;
        
		if (write_frame(settings, s, &pkt, ost->st->codec))
		{
			return -1;
		}
    }
	
	return 0;
}

static int do_subtitle_out(STMTranscodeSettings *settings,
							AVFormatContext *s,
                            AVOutputStream *ost,
                            AVInputStream *ist,
                            AVSubtitle *sub,
                            int64_t pts)
{
    static uint8_t *subtitle_out = NULL;
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, nb, i;
    AVCodecContext *enc;
    AVPacket pkt;

    if (pts == AV_NOPTS_VALUE) {
		if (verbose > 0)
	        fprintf(stderr, "Subtitle packets must have a pts\n");
        return 0;
    }

    enc = ost->st->codec;

    if (!subtitle_out) {
        subtitle_out = av_malloc(subtitle_out_max_size);
    }

    /* Note: DVB subtitle need one packet to draw them and one other
       packet to clear them */
    /* XXX: signal it in the codec context ? */
    if (enc->codec_id == CODEC_ID_DVB_SUBTITLE)
        nb = 2;
    else
        nb = 1;

    for(i = 0; i < nb; i++) {
        sub->pts = av_rescale_q(pts, ist->st->time_base, AV_TIME_BASE_Q);
        // start_display_time is required to be 0
        sub->pts              += av_rescale_q(sub->start_display_time, (AVRational){1, 1000}, AV_TIME_BASE_Q);
        sub->end_display_time -= sub->start_display_time;
        sub->start_display_time = 0;
        subtitle_out_size = avcodec_encode_subtitle(enc, subtitle_out,
                                                    subtitle_out_max_size, sub);
        if (subtitle_out_size < 0) {
            fprintf(stderr, "Subtitle encoding failed\n");
            return -1;
        }

        av_init_packet(&pkt);
        pkt.stream_index = ost->index;
        pkt.data = subtitle_out;
        pkt.size = subtitle_out_size;
        pkt.pts = av_rescale_q(sub->pts, AV_TIME_BASE_Q, ost->st->time_base);
        if (enc->codec_id == CODEC_ID_DVB_SUBTITLE) {
            /* XXX: the pts correction is handled here. Maybe handling
               it in the codec would be better */
            if (i == 0)
                pkt.pts += 90 * sub->start_display_time;
            else
                pkt.pts += 90 * sub->end_display_time;
        }

	    if (write_frame(settings, s, &pkt, ost->st->codec))
		{
			return -1;
		}
    }
	
	return 0;
}

static int bit_buffer_size= 1024*256;
static uint8_t *bit_buffer= NULL;

static int do_video_out(STMTranscodeSettings *settings,
						 AVFormatContext *s,
                         AVOutputStream *ost,
                         AVInputStream *ist,
                         AVFrame *in_picture,
                         int *frame_size)
{
    int nb_frames, i, ret;
    AVFrame *final_picture, *formatted_picture, *resampling_dst, *padding_src;
    AVFrame picture_crop_temp, picture_pad_temp;
    AVCodecContext *enc, *dec;

    avcodec_get_frame_defaults(&picture_crop_temp);
    avcodec_get_frame_defaults(&picture_pad_temp);

    enc = ost->st->codec;
    dec = ist->st->codec;

    /* by default, we output a single frame */
    nb_frames = 1;

    *frame_size = 0;

	double vdelta;
	vdelta = get_sync_ipts(ost, settings) / av_q2d(enc->time_base) - ost->sync_opts;
	//FIXME set to 0.5 after we fix some dts/pts bugs like in avidec.c
	if (vdelta < -1.1 && !settings->output_padding)
		nb_frames = 0;
	else if (s->oformat->flags & AVFMT_VARIABLE_FPS){
		if(vdelta<=-0.6){
			nb_frames=0;
		}else if(vdelta>0.6)
		ost->sync_opts= lrintf(get_sync_ipts(ost, settings) / av_q2d(enc->time_base));
	}else if (vdelta > 1.1)
		nb_frames = lrintf(vdelta);
	if (nb_frames == 0){
		++nb_frames_drop;
		if (verbose>2)
			fprintf(stderr, "*** drop!\n");
	}else if (nb_frames > 1) {
		nb_frames_dup += nb_frames - 1;
		if (verbose>2)
			fprintf(stderr, "*** %d dup!\n", nb_frames-1);
	}

	// limit to 1 frame for thumbnails
    nb_frames= FFMIN(nb_frames, (settings->output_single_frame_only ? 1 : INT_MAX) - ost->frame_number);
    if (nb_frames <= 0)
        return 0;

    formatted_picture = in_picture;

    final_picture = formatted_picture;
    padding_src = formatted_picture;
    resampling_dst = &ost->pict_tmp;

    if(    (ost->resample_height != ist->st->codec->height)
        || (ost->resample_width  != ist->st->codec->width)
        || (ost->resample_pix_fmt!= ist->st->codec->pix_fmt) ) {

		if (verbose > 0)
	        fprintf(stderr,"Input Stream #%d.%d frame size changed to %dx%d, %s\n", ist->file_index, ist->index, ist->st->codec->width,     ist->st->codec->height,avcodec_get_pix_fmt_name(ist->st->codec->pix_fmt));
        if(!ost->video_resample)
            return -1;
    }

    /* duplicates frame if needed */
    for(i=0;i<nb_frames;i++) {
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.stream_index= ost->index;

        if (s->oformat->flags & AVFMT_RAWPICTURE) {
            /* raw pictures are written as AVPicture structure to
               avoid any copies. We support temorarily the older
               method. */
            AVFrame* old_frame = enc->coded_frame;
            enc->coded_frame = dec->coded_frame; //FIXME/XXX remove this hack
            pkt.data= (uint8_t *)final_picture;
            pkt.size=  sizeof(AVPicture);
            pkt.pts= av_rescale_q(ost->sync_opts, enc->time_base, ost->st->time_base);
            pkt.flags |= PKT_FLAG_KEY;

            if (write_frame(settings, s, &pkt, ost->st->codec))
			{
				return -1;
			}
            enc->coded_frame = old_frame;
        } else {
            AVFrame big_picture;

            big_picture= *final_picture;
            /* better than nothing: use input picture interlaced
               settings */
            big_picture.interlaced_frame = in_picture->interlaced_frame;

            /* handles sameq here. This is not correct because it may
               not be a global option */
			big_picture.quality = ost->st->quality;
                big_picture.pict_type = 0;
//            big_picture.pts = AV_NOPTS_VALUE;
            big_picture.pts= ost->sync_opts;
//            big_picture.pts= av_rescale(ost->sync_opts, AV_TIME_BASE*(int64_t)enc->time_base.num, enc->time_base.den);
//av_log(NULL, AV_LOG_DEBUG, "%"PRId64" -> encoder\n", ost->sync_opts);
            ret = avcodec_encode_video(enc,
                                       bit_buffer, bit_buffer_size,
                                       &big_picture);
            if (ret < 0) {
                fprintf(stderr, "Video encoding failed\n");
                return -1;
            }

            if(ret>0){
                pkt.data= bit_buffer;
                pkt.size= ret;
                if(enc->coded_frame->pts != AV_NOPTS_VALUE)
                    pkt.pts= av_rescale_q(enc->coded_frame->pts, enc->time_base, ost->st->time_base);

                if(enc->coded_frame->key_frame)
                    pkt.flags |= PKT_FLAG_KEY;
                
				if (write_frame(settings, s, &pkt, ost->st->codec))
				{
					return -1;
				}
                *frame_size = ret;
                video_size += ret;
            }
        }
		ost->sync_opts++;
        ost->frame_number++;
    }
	return 0;
}

static double psnr(double d){
    return -10.0*log(d)/log(10.0);
}

static void print_report(AVFormatContext **output_files,
                         AVOutputStream **ost_table, int nb_ostreams,
                         int is_last_report)
{
    char buf[1024];
    AVOutputStream *ost;
    AVFormatContext *oc;
    int64_t total_size;
    AVCodecContext *enc;
    int frame_number, vid, i;
    double bitrate, ti1, pts;
    static int64_t last_time = -1;

    if (!is_last_report) {
        int64_t cur_time;
        /* display the report every 0.5 seconds */
        cur_time = av_gettime();
        if (last_time == -1) {
            last_time = cur_time;
            return;
        }
        if ((cur_time - last_time) < 500000)
            return;
        last_time = cur_time;
    }


    oc = output_files[0];

    total_size = url_fsize(oc->pb);
    if(total_size<0) // FIXME improve url_fsize() so it works with non seekable output too
        total_size= url_ftell(oc->pb);
	total_size += file_size;


    buf[0] = '\0';
    ti1 = 1e10;
    vid = 0;
    for(i=0;i<nb_ostreams;i++) {
        ost = ost_table[i];
        enc = ost->st->codec;
        if (vid && enc->codec_type == CODEC_TYPE_VIDEO) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "q=%2.1f ",
                     !ost->st->stream_copy ?
                     enc->coded_frame->quality/(float)FF_QP2LAMBDA : -1);
        }
        if (!vid && enc->codec_type == CODEC_TYPE_VIDEO) {
            float t = (av_gettime()-timer_start) / 1000000.0;

            frame_number = ost->frame_number;
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "frame=%5d fps=%3d q=%3.1f ",
                     frame_number, (t>1)?(int)(frame_number/t+0.5) : 0,
                     !ost->st->stream_copy ?
                     enc->coded_frame->quality/(float)FF_QP2LAMBDA : -1);
            if(is_last_report)
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "L");
            if (enc->flags&CODEC_FLAG_PSNR){
                int j;
                double error, error_sum=0;
                double scale, scale_sum=0;
                char type[3]= {'Y','U','V'};
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "PSNR=");
                for(j=0; j<3; j++){
                    if(is_last_report){
                        error= enc->error[j];
                        scale= enc->width*enc->height*255.0*255.0*frame_number;
                    }else{
                        error= enc->coded_frame->error[j];
                        scale= enc->width*enc->height*255.0*255.0;
                    }
                    if(j) scale/=4;
                    error_sum += error;
                    scale_sum += scale;
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%c:%2.2f ", type[j], psnr(error/scale));
                }
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "*:%2.2f ", psnr(error_sum/scale_sum));
            }
            vid = 1;
        }
        /* compute min output value */
        pts = (double)ost->st->pts.val * av_q2d(ost->st->time_base);
        if ((pts < ti1) && (pts > 0))
            ti1 = pts;
    }
    if (ti1 < 0.01)
        ti1 = 0.01;

    if (verbose || is_last_report) {
        bitrate = (double)(total_size * 8) / ti1 / 1000.0;

        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
            "size=%8.0fkB time=%0.2f bitrate=%6.1fkbits/s",
            (double)total_size / 1024, ti1, bitrate);

        if (nb_frames_dup || nb_frames_drop)
          snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " dup=%d drop=%d",
                  nb_frames_dup, nb_frames_drop);

        if (verbose >= 0)
            fprintf(stderr, "%s    \r", buf);

        fflush(stderr);
    }

    if (is_last_report && verbose >= 0){
        int64_t raw= audio_size + video_size + extra_size;
        fprintf(stderr, "\n");
        fprintf(stderr, "video:%1.0fkB audio:%1.0fkB global headers:%1.0fkB muxing overhead %f%%\n",
                video_size/1024.0,
                audio_size/1024.0,
                extra_size/1024.0,
                100.0*(total_size - raw)/raw
        );
    }
}

/* pkt = NULL means EOF (needed to flush decoder buffers) */
static int output_packet(
	STMTranscodeContext *context,
	AVInputStream *ist,
	int ist_index,
    AVOutputStream **ost_table,
	int nb_ostreams,
	const AVPacket *pkt)
{
	static short *samples;
	STMTranscodeSettings *settings = &context->settings;
    AVFormatContext *os;
    AVOutputStream *ost;
    int ret, i;
    int got_picture;
    AVFrame picture;
    static unsigned int samples_size= 0;
    AVSubtitle subtitle, *subtitle_to_free;
    int got_subtitle;
    int loop;
    AVPacket avpkt;
    int bps = av_get_bits_per_sample_format(ist->st->codec->sample_fmt)>>3;

    if(ist->next_pts == AV_NOPTS_VALUE)
        ist->next_pts= ist->pts;

    if (pkt == NULL) {
        /* EOF handling */
        av_init_packet(&avpkt);
        avpkt.data = NULL;
        avpkt.size = 0;
        goto handle_eof;
    } else {
        avpkt = *pkt;
    }

    if(pkt->dts != AV_NOPTS_VALUE)
        ist->next_pts = ist->pts = av_rescale_q(pkt->dts, ist->st->time_base, AV_TIME_BASE_Q);

    //while we have more to decode or while the decoder did output something on EOF
    while (avpkt.size > 0 || (!pkt && ist->next_pts != ist->pts)) {
        uint8_t *data_buf, *decoded_data_buf;
        int data_size, decoded_data_size;
    handle_eof:
        ist->pts= ist->next_pts;

//        if(avpkt.size && avpkt.size != pkt->size &&
//           !(ist->st->codec->codec->capabilities & CODEC_CAP_SUBFRAMES) && verbose>0)
//            fprintf(stderr, "Multiple frames in a packet from stream %d\n", pkt->stream_index);

        /* decode the packet if needed */
        decoded_data_buf = NULL; /* fail safe */
        decoded_data_size= 0;
        data_buf  = avpkt.data;
        data_size = avpkt.size;
        subtitle_to_free = NULL;
        if (ist->decoding_needed) {
            switch(ist->st->codec->codec_type) {
            case CODEC_TYPE_AUDIO:{
                if(pkt && samples_size < FFMAX(pkt->size*sizeof(*samples), AVCODEC_MAX_AUDIO_FRAME_SIZE)) {
                    samples_size = FFMAX(pkt->size*sizeof(*samples), AVCODEC_MAX_AUDIO_FRAME_SIZE);
                    av_free(samples);
                    samples= av_malloc(samples_size);
                }
                decoded_data_size= samples_size;
                    /* XXX: could avoid copy if PCM 16 bits with same
                       endianness as CPU */
                ret = avcodec_decode_audio3(ist->st->codec, samples, &decoded_data_size,
                                            &avpkt);
                if (ret < 0)
                    goto fail_decode;
                avpkt.data += ret;
                avpkt.size -= ret;
                data_size   = ret;
                /* Some bug in mpeg audio decoder gives */
                /* decoded_data_size < 0, it seems they are overflows */
                if (decoded_data_size <= 0) {
                    /* no audio frame */
                    continue;
                }
                decoded_data_buf = (uint8_t *)samples;
                ist->next_pts += ((int64_t)AV_TIME_BASE/bps * decoded_data_size) /
                    (ist->st->codec->sample_rate * ist->st->codec->channels);
                break;}
            case CODEC_TYPE_VIDEO:
                    decoded_data_size = (ist->st->codec->width * ist->st->codec->height * 3) / 2;
                    /* XXX: allocate picture correctly */
                    avcodec_get_frame_defaults(&picture);

                    ret = avcodec_decode_video2(ist->st->codec,
                                                &picture, &got_picture, &avpkt);
                    ist->st->quality= picture.quality;
                    if (ret < 0)
                        goto fail_decode;
                    if (!got_picture) {
                        /* no picture yet */
                        goto discard_packet;
                    }
                    if (ist->st->codec->time_base.num != 0) {
                        int ticks= ist->st->parser ? ist->st->parser->repeat_pict+1 : ist->st->codec->ticks_per_frame;
                        ist->next_pts += ((int64_t)AV_TIME_BASE *
                                          ist->st->codec->time_base.num * ticks) /
                            ist->st->codec->time_base.den;
                    }
                    avpkt.size = 0;
                    break;
            case CODEC_TYPE_SUBTITLE:
                ret = avcodec_decode_subtitle2(ist->st->codec,
                                               &subtitle, &got_subtitle, &avpkt);
                if (ret < 0)
                    goto fail_decode;
                if (!got_subtitle) {
                    goto discard_packet;
                }
                subtitle_to_free = &subtitle;
                avpkt.size = 0;
                break;
            default:
                goto fail_decode;
            }
        } else {
            switch(ist->st->codec->codec_type) {
            case CODEC_TYPE_AUDIO:
                ist->next_pts += ((int64_t)AV_TIME_BASE * ist->st->codec->frame_size) /
                    ist->st->codec->sample_rate;
                break;
            case CODEC_TYPE_VIDEO:
                if (ist->st->codec->time_base.num != 0) {
                    int ticks= ist->st->parser ? ist->st->parser->repeat_pict+1 : ist->st->codec->ticks_per_frame;
                    ist->next_pts += ((int64_t)AV_TIME_BASE *
                                      ist->st->codec->time_base.num * ticks) /
                        ist->st->codec->time_base.den;
                }
                break;
            }
            ret = avpkt.size;
            avpkt.size = 0;
        }

        if (ist->pts >= 0 &&
			ist->st->codec->codec_type == CODEC_TYPE_VIDEO &&
			ist->input_video_filter) {
            // add it to be filtered
            av_vsrc_buffer_add_frame(ist->input_video_filter, &picture,
                                     ist->pts,
                                     ist->st->codec->sample_aspect_ratio);
        }

        // preprocess audio (volume)
        if (ist->st->codec->codec_type == CODEC_TYPE_AUDIO) {
            if (settings->audio_volume != 256) {
                short *volp;
                volp = samples;
                for(i=0;i<(decoded_data_size / sizeof(short));i++) {
                    int v = ((*volp) * settings->audio_volume + 128) >> 8;
                    if (v < -32768) v = -32768;
                    if (v >  32767) v = 32767;
                    *volp++ = v;
                }
            }
        }

        loop = ist->st->codec->codec_type != CODEC_TYPE_VIDEO ||
            !ist->out_video_filter || avfilter_poll_frame(ist->out_video_filter->inputs[0]);

        /* if output time reached then transcode raw format,
           encode packets and output them */
        if (ist->pts >= 0)
		{
			while(loop) {
				if (ist->st->codec->codec_type == CODEC_TYPE_VIDEO && ist->out_video_filter)
					get_filtered_video_pic(ist->out_video_filter, &ist->picref, &picture, (uint64_t *)&ist->pts);
				for(i=0;i<nb_ostreams;i++) {
					int frame_size;

					ost = ost_table[i];
					if (ost->source_index == ist_index) {
						os = context->output_files[ost->file_index];

						/* set the input output pts pairs */
						//ost->sync_ipts = (double)(ist->pts + input_files_ts_offset[ist->file_index] - start_time)/ AV_TIME_BASE;

						if (ost->encoding_needed) {
							assert(ist->decoding_needed);
							switch(ost->st->codec->codec_type) {
							case CODEC_TYPE_AUDIO:
								if (do_audio_out(settings, os, ost, ist, decoded_data_buf, decoded_data_size))
								{
									return -1;
								}
								break;
							case CODEC_TYPE_VIDEO:
								ost->st->codec->sample_aspect_ratio = ist->picref->pixel_aspect;
								if (do_video_out(settings, os, ost, ist, &picture, &frame_size))
								{
									return -1;
								}
								break;
							case CODEC_TYPE_SUBTITLE:
								if (do_subtitle_out(settings, os, ost, ist, &subtitle,
												pkt->pts))
								{
									return -1;
								}
								break;
							default:
								abort();
							}
						} else {
							/* If we're outputting subtitles, pass discarded subtitle packets of the
							   appropriate stream index to the subtitle renderer */
							if (context->inlineass_context &&
								ist->st->codec->codec_type == CODEC_TYPE_SUBTITLE &&
								pkt->stream_index == context->settings.subtitle_stream_index)
							{
								vf_inlineass_append_data(
									context->inlineass_context,
									ist->st->codec->codec_id,
									(char *)pkt->data,
									pkt->size,
									av_rescale_q(pkt->pts, ist->st->time_base, AV_TIME_BASE_Q) + context->settings.input_start_time,
									av_rescale_q(pkt->convergence_duration ? pkt->convergence_duration : pkt->duration, ist->st->time_base, AV_TIME_BASE_Q),
									AV_TIME_BASE_Q);
							}

							AVFrame avframe; //FIXME/XXX remove this
							AVPacket opkt;
							int64_t ost_tb_start_time= av_rescale_q(0, AV_TIME_BASE_Q, ost->st->time_base);

							av_init_packet(&opkt);

							if ((!ost->frame_number && !(pkt->flags & PKT_FLAG_KEY)))
								continue;

							/* no reencoding needed : output the packet directly */
							/* force the input stream PTS */

							avcodec_get_frame_defaults(&avframe);
							ost->st->codec->coded_frame= &avframe;
							avframe.key_frame = pkt->flags & PKT_FLAG_KEY;

							if(ost->st->codec->codec_type == CODEC_TYPE_AUDIO)
								audio_size += data_size;
							else if (ost->st->codec->codec_type == CODEC_TYPE_VIDEO) {
								video_size += data_size;
								ost->sync_opts++;
							}

							opkt.stream_index= ost->index;
							if(pkt->pts != AV_NOPTS_VALUE)
								opkt.pts= av_rescale_q(pkt->pts, ist->st->time_base, ost->st->time_base) - ost_tb_start_time;
							else
								opkt.pts= AV_NOPTS_VALUE;

							if (pkt->dts == AV_NOPTS_VALUE)
								opkt.dts = av_rescale_q(ist->pts, AV_TIME_BASE_Q, ost->st->time_base);
							else
								opkt.dts = av_rescale_q(pkt->dts, ist->st->time_base, ost->st->time_base);
							opkt.dts -= ost_tb_start_time;

							opkt.duration = av_rescale_q(pkt->duration, ist->st->time_base, ost->st->time_base);
							opkt.flags= pkt->flags;

							//FIXME remove the following 2 lines they shall be replaced by the bitstream filters
							if(ost->st->codec->codec_id != CODEC_ID_H264) {
								if(av_parser_change(ist->st->parser, ost->st->codec, &opkt.data, &opkt.size, data_buf, data_size, pkt->flags & PKT_FLAG_KEY))
									opkt.destruct= av_destruct_packet;
							} else {
								opkt.data = data_buf;
								opkt.size = data_size;
							}

							if (write_frame(settings, os, &opkt, ost->st->codec))
							{
								return -1;
							}
							ost->st->codec->frame_number++;
							ost->frame_number++;
							av_free_packet(&opkt);
						}
					}
					loop =  (ist->st->codec->codec_type == CODEC_TYPE_VIDEO) &&
							ist->out_video_filter && avfilter_poll_frame(ist->out_video_filter->inputs[0]);
				}
				if(ist->picref)
					avfilter_unref_pic(ist->picref);
			}
		}

        /* XXX: allocate the subtitles in the codec ? */
        if (subtitle_to_free) {
            if (subtitle_to_free->rects != NULL) {
                for (i = 0; i < subtitle_to_free->num_rects; i++) {
                    av_freep(&subtitle_to_free->rects[i]->pict.data[0]);
                    av_freep(&subtitle_to_free->rects[i]->pict.data[1]);
                    av_freep(&subtitle_to_free->rects[i]);
                }
                av_freep(&subtitle_to_free->rects);
            }
            subtitle_to_free->num_rects = 0;
            subtitle_to_free = NULL;
        }
    }
 discard_packet:
    if (pkt == NULL) {
        /* EOF handling */

        for(i=0;i<nb_ostreams;i++) {
            ost = ost_table[i];
            if (ost->source_index == ist_index) {
                AVCodecContext *enc= ost->st->codec;
                os = context->output_files[ost->file_index];

                if(ost->st->codec->codec_type == CODEC_TYPE_AUDIO && enc->frame_size <=1)
                    continue;
                if(ost->st->codec->codec_type == CODEC_TYPE_VIDEO && (os->oformat->flags & AVFMT_RAWPICTURE))
                    continue;

                if (ost->encoding_needed) {
                    for(;;) {
                        AVPacket pkt;
                        int fifo_bytes;
                        av_init_packet(&pkt);
                        pkt.stream_index= ost->index;

                        switch(ost->st->codec->codec_type) {
                        case CODEC_TYPE_AUDIO:
                            fifo_bytes = av_fifo_size(ost->fifo);
                            ret = 0;
                            /* encode any samples remaining in fifo */
                            if (fifo_bytes > 0) {
                                int osize = av_get_bits_per_sample_format(enc->sample_fmt) >> 3;
                                int fs_tmp = enc->frame_size;

                                av_fifo_generic_read(ost->fifo, samples, fifo_bytes, NULL);
                                if (enc->codec->capabilities & CODEC_CAP_SMALL_LAST_FRAME) {
                                    enc->frame_size = fifo_bytes / (osize * enc->channels);
                                } else { /* pad */
                                    int frame_bytes = enc->frame_size*osize*enc->channels;
                                    if (samples_size < frame_bytes)
                                        return -1;
                                    memset((uint8_t*)samples+fifo_bytes, 0, frame_bytes - fifo_bytes);
                                }

                                ret = avcodec_encode_audio(enc, bit_buffer, bit_buffer_size, samples);
                                pkt.duration = av_rescale((int64_t)enc->frame_size*ost->st->time_base.den,
                                                          ost->st->time_base.num, enc->sample_rate);
                                enc->frame_size = fs_tmp;
                            }
                            if(ret <= 0) {
                                ret = avcodec_encode_audio(enc, bit_buffer, bit_buffer_size, NULL);
                            }
                            if (ret < 0) {
                                fprintf(stderr, "Audio encoding failed\n");
                                return -1;
                            }
                            audio_size += ret;
                            pkt.flags |= PKT_FLAG_KEY;
                            break;
                        case CODEC_TYPE_VIDEO:
                            ret = avcodec_encode_video(enc, bit_buffer, bit_buffer_size, NULL);
                            if (ret < 0) {
                                fprintf(stderr, "Video encoding failed\n");
                                return -1;
                            }
                            video_size += ret;
                            if(enc->coded_frame && enc->coded_frame->key_frame)
                                pkt.flags |= PKT_FLAG_KEY;
                            break;
                        default:
                            ret=-1;
                        }

                        if(ret<=0)
                            break;
                        pkt.data= bit_buffer;
                        pkt.size= ret;
                        if(enc->coded_frame && enc->coded_frame->pts != AV_NOPTS_VALUE)
                            pkt.pts= av_rescale_q(enc->coded_frame->pts, enc->time_base, ost->st->time_base);
                        if (write_frame(settings, os, &pkt, ost->st->codec))
						{
							return -1;
						}
                    }
                }
            }
        }
    }

    return 0;
 fail_decode:
    return -1;
}

static char *newTitleForFile(AVFormatContext *input_file, int *isFilename)
{
	char *filename = input_file->filename;
	int filenameLength = strlen(filename);
	
	AVMetadata *metadata = input_file->metadata;
	AVMetadataTag *itemTag =
		av_metadata_get(metadata, "title", NULL, 0);
	if (!itemTag || itemTag->value == NULL || strlen(itemTag->value) == 0)
	{
		itemTag =
			av_metadata_get(metadata, "TIT2", NULL, 0);
	}
	if (!itemTag || itemTag->value == NULL || strlen(itemTag->value) == 0)
	{
		itemTag =
			av_metadata_get(metadata, "TT2", NULL, 0);
	}
	int itemLength;
	char *item;
	if (!itemTag || itemTag->value == NULL || strlen(itemTag->value) == 0)
	{
		int i = filenameLength - 1;
		while (filename[i] != '/')
		{
			i--;
		}
		item = &filename[i+1];
		itemLength = filenameLength - i - 1;
		
		if (isFilename)
		{
			*isFilename = 1;
		}
	}
	else
	{
		item = itemTag->value;
		itemLength = strlen(item);
		
		if (isFilename)
		{
			*isFilename = 0;
		}
	}
	
	char *result = av_malloc(sizeof(char) * itemLength + 1);
	memcpy(result, item, itemLength);
	result[itemLength] = '\0';
	
	return result;
}

static char *newArtistForFile(AVFormatContext *input_file, int *isDirname)
{
	char *filename = input_file->filename;
	int filenameLength = strlen(filename);
	
	AVMetadata *metadata = input_file->metadata;
	AVMetadataTag *groupTag =
		av_metadata_get(metadata, "author", NULL, 0);
	if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
	{
		groupTag =
			av_metadata_get(metadata, "TPE1", NULL, 0);
	}
	if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
	{
		groupTag =
			av_metadata_get(metadata, "TP1", NULL, 0);
	}
	if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
	{
		groupTag =
			av_metadata_get(metadata, "show", NULL, 0);
	}
	int groupLength;
	char *group;
	if (!groupTag || groupTag->value == NULL || strlen(groupTag->value) == 0)
	{
		int i = filenameLength - 1;
		while (filename[i] != '/')
		{
			i--;
		}
		int j = i - 1;
		while (j > 0 && filename[j] != '/')
		{
			j--;
		}
		if (j == 0)
		{
			group = "";
			groupLength = 0;
		}
		else
		{
			group = &filename[j+1];
			groupLength = i - j - 1;
		}
		
		if (isDirname)
		{
			*isDirname = 1;
		}
	}
	else
	{
		group = groupTag->value;
		groupLength = strlen(group);

		if (isDirname)
		{
			*isDirname = 0;
		}
	}
	
	char *result = av_malloc(sizeof(char) * groupLength + 1);
	memcpy(result, group, groupLength);
	result[groupLength] = '\0';
	
	return result;
}

static int stream_index_from_inputs(AVFormatContext **input_files,
                                    int nb_input_files,
                                    AVInputFile *file_table,
                                    AVInputStream **ist_table,
                                    enum CodecType type,
                                    int programid)
{
    int p, q, z;
    for(z=0; z<nb_input_files; z++) {
        AVFormatContext *ic = input_files[z];
        for(p=0; p<ic->nb_programs; p++) {
            AVProgram *program = ic->programs[p];
            if(program->id != programid)
                continue;
            for(q=0; q<program->nb_stream_indexes; q++) {
                int sidx = program->stream_index[q];
                int ris = file_table[z].ist_index + sidx;
                if(ist_table[ris]->discard && ic->streams[sidx]->codec->codec_type == type)
                    return ris;
            }
        }
    }

    return -1;
}

/*
 * The following code is the main loop of the file converter
 */
static int av_encode(STMTranscodeContext *context)
{
	STMTranscodeSettings *settings = &context->settings;
    int ret = 0, i, j, k, n, nb_istreams = 0, nb_ostreams = 0;
    AVFormatContext *is, *os;
    AVCodecContext *codec, *icodec;
    AVOutputStream *ost, **ost_table = NULL;
    AVInputStream *ist, **ist_table = NULL;
    AVInputFile *file_table;
    char error[1024];
    uint8_t *no_packet;
    int no_packet_count=0;

	no_packet = av_mallocz(sizeof(uint8_t) * MAX(context->num_input_files, context->num_output_files));

    file_table= av_mallocz(context->num_input_files * sizeof(AVInputFile));
    if (!file_table)
        goto fail;

    /* input stream init */
    j = 0;
    for(i=0;i<context->num_input_files;i++) {
        is = context->input_files[i];
        file_table[i].ist_index = j;
        file_table[i].nb_streams = is->nb_streams;
        j += is->nb_streams;
    }
    nb_istreams = j;

    ist_table = av_mallocz(nb_istreams * sizeof(AVInputStream *));
    if (!ist_table)
        goto fail;

    for(i=0;i<nb_istreams;i++) {
        ist = av_mallocz(sizeof(AVInputStream));
        if (!ist)
            goto fail;
        ist_table[i] = ist;
    }
    j = 0;
    for(i=0;i<context->num_input_files;i++) {
        is = context->input_files[i];
        for(k=0;k<is->nb_streams;k++) {
            ist = ist_table[j++];
            ist->st = is->streams[k];
            ist->file_index = i;
            ist->index = k;
            ist->discard = 1; /* the stream is discarded by default
                                 (changed later) */
        }
    }

    /* output stream init */
    nb_ostreams = 0;
    for(i=0;i<context->num_output_files;i++) {
        os = context->output_files[i];
        if (!os->nb_streams) {
            dump_format(context->output_files[i], i, context->output_files[i]->filename, 1);
            fprintf(stderr, "Output file #%d does not contain any stream\n", i);
            return -1;
        }
        nb_ostreams += os->nb_streams;
    }
    if (context->num_stream_maps > 0 && context->num_stream_maps != nb_ostreams) {
        fprintf(stderr, "Number of stream maps must match number of output streams\n");
        return -1;
    }

    /* Sanity check the mapping args -- do the input files & streams exist? */
    for(i=0;i<context->num_stream_maps;i++) {
        int fi = context->stream_maps[i].file_index;
        int si = context->stream_maps[i].stream_index;

        if (fi < 0 || fi > context->num_input_files - 1 ||
            si < 0 || si > file_table[fi].nb_streams - 1) {
            fprintf(stderr,"Could not find input stream #%d.%d\n", fi, si);
            return -1;
        }
        fi = context->stream_maps[i].sync_file_index;
        si = context->stream_maps[i].sync_stream_index;
        if (fi < 0 || fi > context->num_input_files - 1 ||
            si < 0 || si > file_table[fi].nb_streams - 1) {
            fprintf(stderr,"Could not find sync stream #%d.%d\n", fi, si);
            return -1;
        }
    }

    ost_table = av_mallocz(sizeof(AVOutputStream *) * nb_ostreams);
    if (!ost_table)
        goto fail;
    for(i=0;i<nb_ostreams;i++) {
        ost = av_mallocz(sizeof(AVOutputStream));
        if (!ost)
            goto fail;
        ost_table[i] = ost;
    }

    n = 0;
    for(k=0;k<context->num_output_files;k++) {
        os = context->output_files[k];
        for(i=0;i<os->nb_streams;i++,n++) {
            int found;
            ost = ost_table[n];
            ost->file_index = k;
            ost->index = i;
            ost->st = os->streams[i];
            if (context->num_stream_maps > 0) {
                ost->source_index = file_table[context->stream_maps[n].file_index].ist_index +
                    context->stream_maps[n].stream_index;

                /* Sanity check that the stream types match */
                if (ist_table[ost->source_index]->st->codec->codec_type != ost->st->codec->codec_type) {
                    int i= ost->file_index;
                    dump_format(context->output_files[i], i, context->output_files[i]->filename, 1);
                    fprintf(stderr, "Codec type mismatch for mapping #%d.%d -> #%d.%d\n",
                        context->stream_maps[n].file_index, context->stream_maps[n].stream_index,
                        ost->file_index, ost->index);
                    return -1;
                }

            } else {
				/* get corresponding input stream index : we select the first one with the right type */
				found = 0;
				for(j=0;j<nb_istreams;j++) {
					ist = ist_table[j];
					if (ist->discard &&
						ist->st->codec->codec_type == ost->st->codec->codec_type) {
						ost->source_index = j;
						found = 1;
						break;
					}
				}

                if (!found) {
					/* try again and reuse existing stream */
					for(j=0;j<nb_istreams;j++) {
						ist = ist_table[j];
						if (ist->st->codec->codec_type == ost->st->codec->codec_type) {
							ost->source_index = j;
							found = 1;
						}
					}
                    if (!found) {
                        int i= ost->file_index;
                        dump_format(context->output_files[i], i, context->output_files[i]->filename, 1);
                        fprintf(stderr, "Could not find input stream matching output stream #%d.%d\n",
                                ost->file_index, ost->index);
                        return -1;
                    }
                }
            }
            ist = ist_table[ost->source_index];
            ist->discard = 0;
            ost->sync_ist = (context->num_stream_maps > 0) ?
                ist_table[file_table[context->stream_maps[n].sync_file_index].ist_index +
                         context->stream_maps[n].sync_stream_index] : ist;
        }
    }

    /* for each output stream, we compute the right encoding parameters */
    for(i=0;i<nb_ostreams;i++) {
        AVMetadataTag *lang;
        ost = ost_table[i];
        os = context->output_files[ost->file_index];
        ist = ist_table[ost->source_index];

        codec = ost->st->codec;
        icodec = ist->st->codec;

        if ((lang=av_metadata_get(ist->st->metadata, "language", NULL, 0))
            &&   !av_metadata_get(ost->st->metadata, "language", NULL, 0))
            av_metadata_set(&ost->st->metadata, "language", lang->value);

        ost->st->disposition = ist->st->disposition;
        codec->bits_per_raw_sample= icodec->bits_per_raw_sample;
        codec->chroma_sample_location = icodec->chroma_sample_location;

        if (ost->st->stream_copy) {
            /* if stream_copy is selected, no need to decode or encode */
            codec->codec_id = icodec->codec_id;
            codec->codec_type = icodec->codec_type;

            if(!codec->codec_tag){
                if(   !os->oformat->codec_tag
                   || av_codec_get_id (os->oformat->codec_tag, icodec->codec_tag) == codec->codec_id
                   || av_codec_get_tag(os->oformat->codec_tag, icodec->codec_id) <= 0)
                    codec->codec_tag = icodec->codec_tag;
            }

            codec->bit_rate = icodec->bit_rate;
            codec->extradata= icodec->extradata;
            codec->extradata_size= icodec->extradata_size;
            if(av_q2d(icodec->time_base)*icodec->ticks_per_frame > av_q2d(ist->st->time_base) && av_q2d(ist->st->time_base) < 1.0/1000){
                codec->time_base = icodec->time_base;
                codec->time_base.num *= icodec->ticks_per_frame;
            }else
                codec->time_base = ist->st->time_base;
            switch(codec->codec_type) {
            case CODEC_TYPE_AUDIO:
                if(settings->audio_volume != 256) {
                    fprintf(stderr,"-acodec copy and -vol are incompatible (frames are not decoded)\n");
                    return -1;
                }
                codec->channel_layout = icodec->channel_layout;
                codec->sample_rate = icodec->sample_rate;
                codec->channels = icodec->channels;
                codec->frame_size = icodec->frame_size;
                codec->block_align= icodec->block_align;
                if(codec->block_align == 1 && codec->codec_id == CODEC_ID_MP3)
                    codec->block_align= 0;
                if(codec->codec_id == CODEC_ID_AC3)
                    codec->block_align= 0;
                break;
            case CODEC_TYPE_VIDEO:
                codec->pix_fmt = icodec->pix_fmt;
                codec->width = icodec->width;
                codec->height = icodec->height;
                codec->has_b_frames = icodec->has_b_frames;
                break;
            case CODEC_TYPE_SUBTITLE:
                codec->width = icodec->width;
                codec->height = icodec->height;
                break;
            default:
                abort();
            }
        } else {
            switch(codec->codec_type) {
            case CODEC_TYPE_AUDIO:
                ost->fifo= av_fifo_alloc(1024);
                if(!ost->fifo)
                    goto fail;
                ost->reformat_pair = MAKE_SFMT_PAIR(SAMPLE_FMT_NONE,SAMPLE_FMT_NONE);
                ost->audio_resample = 1;
                icodec->request_channels = codec->channels;
                ist->decoding_needed = 1;
                ost->encoding_needed = 1;
                break;
            case CODEC_TYPE_VIDEO:
                if (ost->st->codec->pix_fmt == PIX_FMT_NONE) {
                    fprintf(stderr, "Video pixel format is unknown, stream cannot be decoded\n");
                    return -1;
                }
                ost->video_resample = (codec->width != icodec->width) ||
                        (codec->height != icodec->height) ||
                        (codec->pix_fmt != icodec->pix_fmt);
                if (ost->video_resample) {
                    avcodec_get_frame_defaults(&ost->pict_tmp);
                    if(avpicture_alloc((AVPicture*)&ost->pict_tmp, codec->pix_fmt,
                                         codec->width, codec->height)) {
                        fprintf(stderr, "Cannot allocate temp picture, check pix fmt\n");
                        return -1;
                    }
                    ost->img_resample_ctx = sws_getContext(
                            icodec->width,
                            icodec->height,
                            icodec->pix_fmt,
                            codec->width,
                            codec->height,
                            codec->pix_fmt,
                            SWS_LANCZOS, NULL, NULL, NULL);
                    if (ost->img_resample_ctx == NULL) {
                        fprintf(stderr, "Cannot get resampling context\n");
                        return -1;
                    }

                    codec->bits_per_raw_sample= 0;
                }
                ost->resample_height = icodec->height;
                ost->resample_width  = icodec->width;
                ost->resample_pix_fmt= icodec->pix_fmt;
                ost->encoding_needed = 1;
                ist->decoding_needed = 1;

                if (configure_filters(context, ist, ost)) {
                    fprintf(stderr, "Error opening filters!\n");
                    exit(1);
                }
                break;
            case CODEC_TYPE_SUBTITLE:
                ost->encoding_needed = 0;
                ist->decoding_needed = 0;
                break;
            default:
                abort();
                break;
            }
        }
        if(codec->codec_type == CODEC_TYPE_VIDEO){
            int size= codec->width * codec->height;
            bit_buffer_size= FFMAX(bit_buffer_size, 6*size + 200);
        }
    }

    if (!bit_buffer)
        bit_buffer = av_malloc(bit_buffer_size);
    if (!bit_buffer) {
        fprintf(stderr, "Cannot allocate %d bytes output buffer\n",
                bit_buffer_size);
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* open each encoder */
    for(i=0;i<nb_ostreams;i++) {
        ost = ost_table[i];
        if (ost->encoding_needed) {
            AVCodec *codec = context->output_codecs[i];
            if (!codec)
                codec = avcodec_find_encoder(ost->st->codec->codec_id);
            if (!codec) {
                snprintf(error, sizeof(error), "Encoder (codec id %d) not found for output stream #%d.%d",
                         ost->st->codec->codec_id, ost->file_index, ost->index);
                ret = AVERROR(EINVAL);
                goto dump_format;
            }
            if (avcodec_open(ost->st->codec, codec) < 0) {
                snprintf(error, sizeof(error), "Error while opening encoder for output stream #%d.%d - maybe incorrect parameters such as bit_rate, rate, width or height",
                        ost->file_index, ost->index);
                ret = AVERROR(EINVAL);
                goto dump_format;
            }
            extra_size += ost->st->codec->extradata_size;
        }
    }

    /* open each decoder */
    for(i=0;i<nb_istreams;i++) {
        ist = ist_table[i];
        if (ist->decoding_needed) {
            AVCodec *codec = context->input_codecs[i];
            if (!codec)
                codec = avcodec_find_decoder(ist->st->codec->codec_id);
            if (!codec) {
                snprintf(error, sizeof(error), "Decoder (codec id %d) not found for input stream #%d.%d",
                        ist->st->codec->codec_id, ist->file_index, ist->index);
                ret = AVERROR(EINVAL);
                goto dump_format;
            }
            if (avcodec_open(ist->st->codec, codec) < 0) {
                snprintf(error, sizeof(error), "Error while opening decoder for input stream #%d.%d",
                        ist->file_index, ist->index);
                ret = AVERROR(EINVAL);
                goto dump_format;
            }
            //if (ist->st->codec->codec_type == CODEC_TYPE_VIDEO)
            //    ist->st->codec->flags |= CODEC_FLAG_REPEAT_FIELD;
        }
    }

	if (context->inlineass_context)
	{
		if (context->settings.album_art_mode == 1)
		{
			int isDirname;
			int isFilename;
			char *artist = newArtistForFile(context->input_files[0], &isDirname);
			char *title = newTitleForFile(context->input_files[0], &isFilename);
			
			char *subtitle;
			int length;
			
			if (isDirname)
			{
				length = asprintf(&subtitle, "%s//%s", artist, title);
			}
			else
			{
				length = asprintf(&subtitle, "%s\n%s", artist, title);
			}
			
			vf_inlineass_append_data(
				context->inlineass_context,
				0,
				subtitle,
				length,
				context->settings.output_duration,
				0,
				AV_TIME_BASE_Q);
			
			av_free(subtitle);
			av_free(artist);
			av_free(title);
		}
		else
		{
			for(i=0;i<context->num_input_files;i++) {
				is = context->input_files[i];
				for(k=0;k<is->nb_streams;k++) {
					if (i == 0 && k == context->settings.subtitle_stream_index &&
						is->streams[k]->codec->extradata_size)
					{
						vf_inlineass_append_data(
							context->inlineass_context,
							is->streams[k]->codec->codec_id,
							(char *)(is->streams[k]->codec->extradata),
							is->streams[k]->codec->extradata_size,
							-1,
							0,
							is->streams[k]->time_base);
					}
				}
			}
		}
	}

    /* init pts */
    for(i=0;i<nb_istreams;i++) {
        ist = ist_table[i];
        ist->pts = 0;
        ist->next_pts = AV_NOPTS_VALUE;
        ist->is_start = 1;
    }

    /* set meta data information from input file if required */
    for (i=0;i<context->num_meta_data_maps;i++) {
        AVFormatContext *out_file;
        AVFormatContext *in_file;
        AVMetadataTag *mtag;

        int out_file_index = context->meta_data_maps[i].out_file;
        int in_file_index = context->meta_data_maps[i].in_file;
        if (out_file_index < 0 || out_file_index >= context->num_output_files) {
            snprintf(error, sizeof(error), "Invalid output file index %d map_meta_data(%d,%d)",
                     out_file_index, out_file_index, in_file_index);
            ret = AVERROR(EINVAL);
            goto dump_format;
        }
        if (in_file_index < 0 || in_file_index >= context->num_input_files) {
            snprintf(error, sizeof(error), "Invalid input file index %d map_meta_data(%d,%d)",
                     in_file_index, out_file_index, in_file_index);
            ret = AVERROR(EINVAL);
            goto dump_format;
        }

        out_file = context->output_files[out_file_index];
        in_file = context->input_files[in_file_index];


        mtag=NULL;
        while((mtag=av_metadata_get(in_file->metadata, "", mtag, AV_METADATA_IGNORE_SUFFIX)))
            av_metadata_set(&out_file->metadata, mtag->key, mtag->value);
        av_metadata_conv(out_file, out_file->oformat->metadata_conv,
                                    in_file->iformat->metadata_conv);
    }

    /* open files and write file headers */
    for(i=0;i<context->num_output_files;i++) {
        os = context->output_files[i];
        if (av_write_header(os) < 0) {
            snprintf(error, sizeof(error), "Could not write header for output file #%d (incorrect codec parameters ?)", i);
            ret = AVERROR(EINVAL);
            goto dump_format;
        }
    }

 dump_format:
    /* dump the file output parameters - cannot be done before in case
       of stream copy */
    for(i=0;i<context->num_output_files;i++) {
        dump_format(context->output_files[i], i, context->output_files[i]->filename, 1);
    }

    /* dump the stream mapping */
    if (verbose >= 0) {
        fprintf(stderr, "Stream mapping:\n");
        for(i=0;i<nb_ostreams;i++) {
            ost = ost_table[i];
            fprintf(stderr, "  Stream #%d.%d -> #%d.%d",
                    ist_table[ost->source_index]->file_index,
                    ist_table[ost->source_index]->index,
                    ost->file_index,
                    ost->index);
            if (ost->sync_ist != ist_table[ost->source_index])
                fprintf(stderr, " [sync #%d.%d]",
                        ost->sync_ist->file_index,
                        ost->sync_ist->index);
            fprintf(stderr, "\n");
        }
    }

    if (ret) {
        fprintf(stderr, "%s\n", error);
        goto fail;
    }

    timer_start = av_gettime();

    for(;;) {
        int file_index, ist_index;
        AVPacket pkt;
        double ipts_min;
        double opts_min;

    redo:
        ipts_min= 1e100;
        opts_min= 1e100;

        /* select the stream that we must read now by looking at the
           smallest output pts */
        file_index = -1;
        for(i=0;i<nb_ostreams;i++) {
            double ipts, opts;
            ost = ost_table[i];
            os = context->output_files[ost->file_index];
            ist = ist_table[ost->source_index];
            if(no_packet[ist->file_index])
                continue;
            if(ost->st->codec->codec_type == CODEC_TYPE_VIDEO)
                opts = ost->sync_opts * av_q2d(ost->st->codec->time_base);
            else
                opts = ost->st->pts.val * av_q2d(ost->st->time_base);
            ipts = (double)ist->pts;
            if (!file_table[ist->file_index].eof_reached){
                if(opts < opts_min) {
                    opts_min = opts;
                    file_index = ist->file_index;
                }
            }
			// Limit to a single frame for thumbnails
            if(ost->st->codec->codec_type == CODEC_TYPE_VIDEO &&
			   ost->frame_number >= (settings->output_single_frame_only ? 1 : INT_MAX)){
                file_index= -1;
                break;
            }
        }
        /* if none, if is finished */
        if (file_index < 0) {
            if(no_packet_count){
                no_packet_count=0;
                memset(no_packet, 0, sizeof(no_packet));
                usleep(10000);
                continue;
            }
            break;
        }
		
        /* read a frame from it and output it in the fifo */
        is = context->input_files[file_index];
        ret= av_read_frame(is, &pkt);
		
        if(ret == AVERROR(EAGAIN)){
            no_packet[file_index]=1;
            no_packet_count++;
            continue;
        }
        if (ret < 0) {
            file_table[file_index].eof_reached = 1;
			continue;
        }
		
        no_packet_count=0;
        memset(no_packet, 0, sizeof(no_packet));

        /* the following test is needed in case new streams appear
           dynamically in stream : we ignore them */
        if (pkt.stream_index >= file_table[file_index].nb_streams)
            goto discard_packet;
        ist_index = file_table[file_index].ist_index + pkt.stream_index;
        ist = ist_table[ist_index];
        if (ist->discard)
            goto discard_packet;

        if (pkt.dts != AV_NOPTS_VALUE)
            pkt.dts += av_rescale_q(context->input_files_ts_offset[ist->file_index], AV_TIME_BASE_Q, ist->st->time_base);
        if (pkt.pts != AV_NOPTS_VALUE)
            pkt.pts += av_rescale_q(context->input_files_ts_offset[ist->file_index], AV_TIME_BASE_Q, ist->st->time_base);
		
//        fprintf(stderr, "next:%"PRId64" source pts:%"PRId64" dest pts:%"PRId64" %d\n",
//			ist->next_pts,
//			av_rescale_q(pkt.pts, ist->st->time_base, AV_TIME_BASE_Q) - context->input_files_ts_offset[ist->file_index],
//			av_rescale_q(pkt.pts, ist->st->time_base, AV_TIME_BASE_Q),
//			ist->st->codec->codec_type);

        if (pkt.dts != AV_NOPTS_VALUE && ist->next_pts != AV_NOPTS_VALUE
            && (is->iformat->flags & AVFMT_TS_DISCONT)) {
            int64_t pkt_dts= av_rescale_q(pkt.dts, ist->st->time_base, AV_TIME_BASE_Q);
            int64_t delta= pkt_dts - ist->next_pts;
            if((FFABS(delta) > 1LL*settings->output_dts_delta_threshold*AV_TIME_BASE || pkt_dts+1<ist->pts)){
                context->input_files_ts_offset[ist->file_index]-= delta;
                if (verbose > 2)
                    fprintf(stderr, "timestamp discontinuity %"PRId64", new offset= %"PRId64"\n", delta, context->input_files_ts_offset[ist->file_index]);
                pkt.dts-= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
                if(pkt.pts != AV_NOPTS_VALUE)
                    pkt.pts-= av_rescale_q(delta, AV_TIME_BASE_Q, ist->st->time_base);
            }
        }

		if (settings->album_art_mode && file_table[0].eof_reached)
		{
			settings->output_padding = 1;
			int64_t end_ts = context->settings.output_duration;
			if (settings->last_output_pts >= end_ts)
			{
				fprintf(stderr, "End of stream.\n");
				break;
			}
		}

        //fprintf(stderr,"read #%d.%d size=%d\n", ist->file_index, ist->index, pkt.size);
        if (output_packet(context, ist, ist_index, ost_table, nb_ostreams, &pkt) < 0) {

            if (verbose >= 0)
                fprintf(stderr, "Error while decoding stream #%d.%d\n",
                        ist->file_index, ist->index);
            av_free_packet(&pkt);
            goto redo;
        }

    discard_packet:
        av_free_packet(&pkt);

        /* dump report by using the output first video and audio streams */
        print_report(context->output_files, ost_table, nb_ostreams, 0);
    }

    /* at the end of stream, we must flush the decoder buffers */
    for(i=0;i<nb_istreams;i++) {
        ist = ist_table[i];
        if (ist->decoding_needed) {
            output_packet(context, ist, i, ost_table, nb_ostreams, NULL);
        }
    }

	if (verbose > 0)
	{
		fprintf(stderr, "End of regular packets\n");
	}

	if (!context->settings.output_single_frame_only &&
		(video_size > 0 || audio_size > 0))
	{
		AVFormatContext *s = context->output_files[0];
		
		int64_t end_ts = context->settings.output_duration;
		
		while (settings->last_output_pts <= end_ts)
		{
			//
			// Generate packets for each of the output streams
			//
			for (i = 0; i < s->nb_streams; i++)
			{
				AVStream *stream = s->streams[i];
				
				AVPacket pkt;
				av_init_packet(&pkt);
				
				//
				// Send a NULL pack that will be picked up by the custom code in mpegtsenc to
				// generate appropriate NULL packets.
				//
				unsigned char data[5] = {0, 0, 0, 1, 0};
				pkt.data = data;
				pkt.size = 5;
				pkt.stream_index = i;
				
				//
				// We need a PCR every 10th of a second, so output at twice that to
				// remain reliable.
				//
				pkt.pts = av_rescale_q(settings->last_output_pts + 0.05 * AV_TIME_BASE, AV_TIME_BASE_Q, stream->time_base);
				
				if (s->oformat->write_packet(s, &pkt) != 0)
				{
					fprintf(stderr, "Failed to write blank packet\n");
					return -1;
				}

				settings->last_output_pts = av_rescale_q(pkt.pts, stream->time_base, AV_TIME_BASE_Q);

				int64_t segment_end = SegmentEnd(settings);
				
				if (settings->last_output_pts >= segment_end && settings->last_output_pts <= end_ts)
				{
					put_flush_packet(s->pb);
					url_fclose(s->pb);
					
					fprintf(stdout, "Wrote segment %lld\n", context->settings.output_segment_index);
					fflush(stdout);

					context->settings.output_segment_index++;
					UpdateFilePathForSegment(&context->settings);
					
					if (url_fopen(&s->pb, context->settings.output_file_path, URL_WRONLY) < 0)
					{
						fprintf(stderr, "Couldn't open '%s'\n", context->settings.output_file_path);
						s->pb = NULL;
						return -1;
					}
				}
			}
		}
	}

    /* write the trailer if needed and close file */
    for(i=0;i<context->num_output_files;i++) {
        os = context->output_files[i];
        av_write_trailer(os);
    }

    /* dump report by using the first video and audio streams */
    print_report(context->output_files, ost_table, nb_ostreams, 1);

    /* close each encoder */
    for(i=0;i<nb_ostreams;i++) {
        ost = ost_table[i];
        if (ost->encoding_needed) {
            av_freep(&ost->st->codec->stats_in);
            avcodec_close(ost->st->codec);
        }
    }

    /* close each decoder */
    for(i=0;i<nb_istreams;i++) {
        ist = ist_table[i];
        if (ist->decoding_needed) {
            avcodec_close(ist->st->codec);
        }
    }

    if (context->filt_graph_all) {
        avfilter_graph_destroy(context->filt_graph_all);
        av_freep(&context->filt_graph_all);
    }

    /* finished ! */
    ret = 0;

 fail:
 	av_free(no_packet);
    av_freep(&bit_buffer);
    av_free(file_table);

    if (ist_table) {
        for(i=0;i<nb_istreams;i++) {
            ist = ist_table[i];
            av_free(ist);
        }
        av_free(ist_table);
    }
    if (ost_table) {
        for(i=0;i<nb_ostreams;i++) {
            ost = ost_table[i];
            if (ost) {
                av_fifo_free(ost->fifo); /* works even if fifo is not
                                             initialized but set to zero */
                av_free(ost->pict_tmp.data[0]);
                if (ost->video_resample)
                    sws_freeContext(ost->img_resample_ctx);
                if (ost->resample)
                    audio_resample_close(ost->resample);
                if (ost->reformat_ctx)
                    av_audio_convert_free(ost->reformat_ctx);
                av_free(ost);
            }
        }
        av_free(ost_table);
    }
    return ret;
}

static enum CodecID find_codec_or_die(const char *name, int type, int encoder)
{
    const char *codec_string = encoder ? "encoder" : "decoder";
    AVCodec *codec;

    if(!name)
        return CODEC_ID_NONE;
    codec = encoder ?
        avcodec_find_encoder_by_name(name) :
        avcodec_find_decoder_by_name(name);
    if(!codec) {
        fprintf(stderr, "Unknown %s '%s'\n", codec_string, name);
        exit(1);
    }
    if(codec->type != type) {
        fprintf(stderr, "Invalid %s type '%s'\n", codec_string, name);
        exit(1);
    }
    return codec->id;
}

static void check_audio_video_sub_inputs(STMTranscodeContext *context,
	int *has_video_ptr, int *has_audio_ptr, int *has_subtitle_ptr)
{
    int has_video, has_audio, has_subtitle, i, j;
    AVFormatContext *ic;

    has_video = 0;
    has_audio = 0;
    has_subtitle = 0;
    for(j=0;j<context->num_input_files;j++) {
        ic = context->input_files[j];
        for(i=0;i<ic->nb_streams;i++) {
            AVCodecContext *enc = ic->streams[i]->codec;
            switch(enc->codec_type) {
            case CODEC_TYPE_AUDIO:
                has_audio = 1;
                break;
            case CODEC_TYPE_VIDEO:
                has_video = 1;
                break;
            case CODEC_TYPE_SUBTITLE:
                has_subtitle = 1;
                break;
            case CODEC_TYPE_DATA:
            case CODEC_TYPE_ATTACHMENT:
            case CODEC_TYPE_UNKNOWN:
                break;
            default:
                abort();
            }
        }
    }
    *has_video_ptr = has_video;
    *has_audio_ptr = has_audio;
    *has_subtitle_ptr = has_subtitle;
}

static int new_video_stream(STMTranscodeContext *context, AVFormatContext *oc)
{
    AVStream *st;
    AVCodecContext *video_enc;
    enum CodecID codec_id;

    st = av_new_stream(oc, oc->nb_streams);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
		return -1;
    }
    avcodec_get_context_defaults2(st->codec, CODEC_TYPE_VIDEO);

	if (context->settings.video_codec_name &&
		strncmp(context->settings.video_codec_name, "mjpeg", 5) != 0)
	{
        avcodec_thread_init(st->codec, threadCount());
	}
	else
	{
        avcodec_thread_init(st->codec, 1);
	}

    video_enc = st->codec;
	AVCodec *codec;
	AVRational fps;
	
//	if (context->settings.album_art_mode)
//	{
//		fps = (AVRational){4,1};
//	}
//	else
	{
		fps =
			context->settings.video_frame_rate.num ?
				context->settings.video_frame_rate :
					context->settings.source_video_frame_rate.num ?
						context->settings.source_video_frame_rate :
							(AVRational){25,1};
	}

	if (context->settings.video_codec_name) {
		codec_id = find_codec_or_die(context->settings.video_codec_name, CODEC_TYPE_VIDEO, 1);
		codec = avcodec_find_encoder_by_name(context->settings.video_codec_name);
	} else {
		codec_id = av_guess_codec(oc->oformat, NULL, oc->filename, NULL, CODEC_TYPE_VIDEO);
		codec = avcodec_find_encoder(codec_id);
	}
	
	AddNewOutputCodec(context, codec);

	video_enc->codec_id = codec_id;

	if (codec && codec->supported_framerates)
		fps = codec->supported_framerates[av_find_nearest_q_idx(fps, codec->supported_framerates)];
	video_enc->time_base.den = fps.num;
	video_enc->time_base.num = fps.den;

	video_enc->width = context->settings.video_frame_width;
	video_enc->height = context->settings.video_frame_height;
	video_enc->sample_aspect_ratio = av_d2q(context->settings.source_video_aspect_ratio*video_enc->height/video_enc->width, 255);
	video_enc->pix_fmt = PIX_FMT_NONE;
	st->sample_aspect_ratio = video_enc->sample_aspect_ratio;

	if(codec && codec->pix_fmts){
		const enum PixelFormat *p= codec->pix_fmts;
		for(; *p!=-1; p++){
			if(*p == video_enc->pix_fmt)
				break;
		}
		if(*p == -1)
			video_enc->pix_fmt = codec->pix_fmts[0];
	}

	video_enc->rc_override_count=0;
	if (!video_enc->rc_initial_buffer_occupancy)
		video_enc->rc_initial_buffer_occupancy = video_enc->rc_buffer_size*3/4;
	video_enc->me_threshold= 0;
	video_enc->intra_dc_precision= 0;

	STMTranscodeSettings *settings = &context->settings;
	
	if (settings->video_crf > 0)
	{
		av_set_int(video_enc, "crf", settings->video_crf);
	}
	else
	{
		av_set_int(video_enc, "b", settings->video_maxrate);
	}
	av_set_int(video_enc, "level", settings->video_level);
	av_set_int(video_enc, "me_range", settings->video_me_range);
	if (settings->video_me_method)
	{
		av_set_string3(video_enc, "me_method", settings->video_me_method, 0, NULL);
	}
	av_set_int(video_enc, "refs", settings->video_frame_refs);
	av_set_int(video_enc, "sc_threshold", settings->video_sc_threshold);
	av_set_int(video_enc, "qdiff", settings->video_qdiff);
	av_set_int(video_enc, "qmin", settings->video_qmin);
	av_set_int(video_enc, "qmax", settings->video_qmax);
	av_set_double(video_enc, "qcomp", settings->video_qcomp);
	av_set_int(video_enc, "g", settings->video_g);
	av_set_int(video_enc, "keyint_min", settings->video_g / 4);
	av_set_int(video_enc, "coder", 0);
	av_set_double(video_enc, "i_qfactor", 0.71);
	av_set_double(video_enc, "subq", settings->video_subq);
//	av_set_int(video_enc, "bufsize", settings->video_bufsize);
//	av_set_int(video_enc, "minrate", settings->video_minrate);
//	av_set_int(video_enc, "maxrate", settings->video_maxrate);
	av_set_string3(video_enc, "flags", settings->video_flags, 0, NULL);
	av_set_string3(video_enc, "flags2", settings->video_flags2, 0, NULL);
	av_set_string3(video_enc, "cmp", settings->video_cmp, 0, NULL);
	av_set_string3(video_enc, "partitions", settings->video_partitions, 0, NULL);
	
	return 0;
}

static int new_audio_stream(STMTranscodeContext *context, AVFormatContext *oc)
{
    AVStream *st;
    AVCodecContext *audio_enc;
    enum CodecID codec_id;

    st = av_new_stream(oc, oc->nb_streams);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        return -1;
    }
    avcodec_get_context_defaults2(st->codec, CODEC_TYPE_AUDIO);

    avcodec_thread_init(st->codec, threadCount());

    audio_enc = st->codec;
    audio_enc->codec_type = CODEC_TYPE_AUDIO;

	AVCodec *codec;

	if (context->settings.audio_codec_name) {
		codec_id = find_codec_or_die(context->settings.audio_codec_name, CODEC_TYPE_AUDIO, 1);
		codec = avcodec_find_encoder_by_name(context->settings.audio_codec_name);
	} else {
		codec_id = av_guess_codec(oc->oformat, NULL, oc->filename, NULL, CODEC_TYPE_AUDIO);
		codec = avcodec_find_encoder(codec_id);
	}

	AddNewOutputCodec(context, codec);

	audio_enc->codec_id = codec_id;

	audio_enc->channels = context->settings.audio_channels;
	audio_enc->channel_layout = context->settings.source_audio_channel_layout;
	if (avcodec_channel_layout_num_channels(context->settings.source_audio_channel_layout) != context->settings.audio_channels)
		audio_enc->channel_layout = 0;

	if(codec && codec->sample_fmts){
		const enum SampleFormat *p= codec->sample_fmts;
		for(; *p!=-1; p++){
			if(*p == audio_enc->sample_fmt)
				break;
		}
		if(*p == -1)
			audio_enc->sample_fmt = codec->sample_fmts[0];
	}
	
    audio_enc->sample_rate = context->settings.audio_sample_rate;
    audio_enc->time_base= (AVRational){1, context->settings.audio_sample_rate};
	
	av_set_int(audio_enc, "ab", context->settings.audio_bitrate);
	
	return 0;
}

static int new_subtitle_stream(STMTranscodeContext *context, AVFormatContext *oc)
{
    AVStream *st;
    AVCodecContext *subtitle_enc;

    st = av_new_stream(oc, oc->nb_streams);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        return -1;
    }
    avcodec_get_context_defaults2(st->codec, CODEC_TYPE_SUBTITLE);
    avcodec_thread_init(st->codec, 1);

    subtitle_enc = st->codec;
    subtitle_enc->codec_type = CODEC_TYPE_SUBTITLE;
	subtitle_enc->codec_id = CODEC_ID_TEXT;
	
	AddNewOutputCodec(context, NULL);

	return 0;
}

static void av_close(STMTranscodeContext *context)
{
    int i;

    /* close files */
    for(i=0;i<context->num_output_files;i++) {
        /* maybe av_close_output_file ??? */
        AVFormatContext *s = context->output_files[i];
        int j;
        if (!(s->oformat->flags & AVFMT_NOFILE) && s->pb)
            url_fclose(s->pb);
        for(j=0;j<s->nb_streams;j++) {
            av_metadata_free(&s->streams[j]->metadata);
            av_free(s->streams[j]->codec);
            av_free(s->streams[j]);
        }
        for(j=0;j<s->nb_programs;j++) {
            av_metadata_free(&s->programs[j]->metadata);
        }
        for(j=0;j<s->nb_chapters;j++) {
            av_metadata_free(&s->chapters[j]->metadata);
        }
        av_metadata_free(&s->metadata);
        av_free(s);
    }

	if (!context->settings.output_single_frame_only &&
		(video_size > 0 || audio_size > 0))
	{
		fprintf(stdout, "Wrote segment %lld\n",
			context->settings.output_segment_index);
		fflush(stdout);
	}		

    for(i=0;i<context->num_input_files;i++)
        av_close_input_file(context->input_files[i]);

	av_free(context->input_files);
	av_free(context->output_files);
	av_free(context->stream_maps);
	av_free(context->meta_data_maps);
	av_free(context->output_codecs);
	
	av_free(context->input_codecs);
	av_free(context->settings.output_file_path);
	av_free(context->settings.video_filters);

	av_free(subtitle_path);

    avfilter_uninit();
}

static const char *pathExtension(const char *filename)
{
	int i;
	for (i = strlen(filename) - 1; i >= 0; i--)
	{
		if (filename[i] == '.')
		{
			break;
		}
	}
	if (i <= 0)
	{
		return "";
	}
	return &filename[i + 1];
}

static int configureInputFiles(STMTranscodeContext *context)
{
    const char *filename = context->settings.input_file_name;
 	AVFormatParameters params, *ap = &params;
    AVInputFormat *file_iformat = NULL;
    int err, i, ret, rfps, rfps_base;
    int64_t timestamp;

    /* get default parameters from command line */
	AddNewInputFile(context, avformat_alloc_context(), 0);

	AVFormatContext *ic = context->input_files[context->num_input_files - 1];
	
    if (!ic) {
        return -1;
    }

    memset(ap, 0, sizeof(*ap));
    ap->prealloced_context = 1;
    ap->video_codec_id = CODEC_ID_NONE;
    ap->audio_codec_id = CODEC_ID_NONE;

    ic->video_codec_id   = CODEC_ID_NONE;
    ic->audio_codec_id   = CODEC_ID_NONE;
    ic->subtitle_codec_id= CODEC_ID_NONE;
    ic->flags |= AVFMT_FLAG_NONBLOCK;

    /* open the input file with generic libav function */
    err = av_open_input_file(&ic, filename, file_iformat, 0, ap);
    if (err < 0) {
		av_free(ic);
		context->num_input_files--;
        return -1;
    }

    ic->loop_input = context->settings.album_art_mode;

    /* If not enough info to get the stream parameters, we decode the
       first frames to get it. (used in mpeg case for example) */
	if (strcmp(pathExtension(filename), "dvr-ms") == 0)
	{
		ic->max_analyze_duration = 1234567;
	}
	
    ret = av_find_stream_info(ic);
    if (ret < 0 && verbose >= 0) {
        fprintf(stderr, "%s: could not find codec parameters\n", filename);
        return -1;
    }

	if (context->settings.output_single_frame_only && ic->duration > AV_TIME_BASE)
	{
		if (ic->duration > 6 * 60 * AV_TIME_BASE)
		{
			context->settings.input_start_time = 3 * 60 * AV_TIME_BASE;
		}
		else
		{
			context->settings.input_start_time = ic->duration >> 1;
		}
	}
	
    timestamp = context->settings.input_start_time;

	if(strcmp(ic->iformat->name, "mpegts")==0 && strcmp(pathExtension(filename), "mpg") == 0)
	{
		//
		// Workaround hack for EyeTV issues -- skip the first 128k of the file.
		//
		int default_index = av_find_default_stream_index(ic);
		av_seek_frame(ic, default_index, 128 * 1024, AVSEEK_FLAG_BYTE | AVSEEK_FLAG_FRAME);
		AVPacket pkt;
		while (av_read_packet(ic, &pkt) >= 0 && pkt.pts == AV_NOPTS_VALUE)
		{
			// do nothing
		}
		if (pkt.pts != AV_NOPTS_VALUE)
		{
			int64_t start_time = 0;
			if (ic->start_time != AV_NOPTS_VALUE && ic->start_time > 0)
			{
				start_time = ic->start_time;
			}
			
			int64_t excess = 
				pkt.pts * AV_TIME_BASE  *
				ic->streams[default_index]->time_base.num
				/ ic->streams[default_index]->time_base.den;
			timestamp += excess;
			ic->duration -= excess - start_time;
		}
	}

	/* add the stream start time */
	else if (ic->start_time != AV_NOPTS_VALUE && ic->start_time > 0)
	{
		timestamp += ic->start_time;
		ic->duration -= ic->start_time;
	}

    /* if seeking requested, we execute it */
    if (timestamp != 0) {
		if (context->settings.output_initial_segment > 0)
		{
			timestamp -= TRANSITION_REPEAT * AV_TIME_BASE;
		}
        ret = av_seek_frame(ic, -1, timestamp, context->settings.output_single_frame_only ? 0 : AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            fprintf(stderr, "%s: could not seek to position %0.3f\n",
                    filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    /* update the current parameters so that they match the one of the input stream */
    for(i=0;i<ic->nb_streams;i++) {
        AVStream *st = ic->streams[i];
        AVCodecContext *enc = st->codec;
		
        switch(enc->codec_type) {
        case CODEC_TYPE_AUDIO:
			if (context->settings.subtitle_stream_index != -1)
			{
			   AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
				st->discard = AVDISCARD_ALL;
				break;
			}

			avcodec_thread_init(enc, threadCount());
			//fprintf(stderr, "\nInput Audio channels: %d", enc->channels);
			context->settings.source_audio_channel_layout = enc->channel_layout;
			AddNewInputCodec(context, avcodec_find_decoder_by_name(context->settings.audio_codec_name));
			break;
        case CODEC_TYPE_VIDEO:
			if (context->settings.subtitle_stream_index != -1)
			{
				AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
				st->discard = AVDISCARD_ALL;
				break;
			}

			if (enc->codec_id != CODEC_ID_SVQ3 &&
				enc->codec_id != CODEC_ID_SVQ3)
			{
				avcodec_thread_init(enc, threadCount());
			}
			else
			{
				avcodec_thread_init(enc, 1);
			}

			if (context->settings.album_art_mode)
			{
				//
				// Set a flag so that we can enable "single frame" mode in the
				// image decoders
				//
				enc->flags |= CODEC_FLAG2_NO_OUTPUT;
			}

			if (enc->width == 0 || enc->height == 0 || enc->width > 10000 || enc->height > 10000)
			{
				if (verbose > 0)
				{
					fprintf(stderr,"Invalid frame size detected\n");
				}
				av_free(ic);
				context->num_input_files--;
				return -1;
			}

			context->settings.source_frame_width = enc->width;
			context->settings.source_frame_height = enc->height;

			if(st->sample_aspect_ratio.num)
			{
				context->settings.source_video_aspect_ratio=av_q2d(st->sample_aspect_ratio);
			}
			else
			{
				context->settings.source_video_aspect_ratio=av_q2d(enc->sample_aspect_ratio);
				if (context->settings.source_video_aspect_ratio == 0)
				{
					context->settings.source_video_aspect_ratio = 1.0;
				}
			}
			context->settings.source_video_aspect_ratio *= (float) enc->width / enc->height;

			rfps      = st->r_frame_rate.num;
			rfps_base = st->r_frame_rate.den;

			if (enc->time_base.den != rfps || enc->time_base.num != rfps_base) {

				if (verbose >= 0)
					fprintf(stderr,"\nSeems stream %d codec frame rate differs from container frame rate: %2.2f (%d/%d) -> %2.2f (%d/%d)\n",
							i, (float)enc->time_base.den / enc->time_base.num, enc->time_base.den, enc->time_base.num,

					(float)rfps / rfps_base, rfps, rfps_base);
			}
						
			/* update the current frame rate to match the stream frame rate
			   rounded to the nearest multiple of 3 */
			context->settings.source_video_frame_rate.num =
				3 * lround((rfps / (double)rfps_base) * 0.333333333);
			context->settings.source_video_frame_rate.den = 1;

			double avg_fps = st->avg_frame_rate.num / (double)st->avg_frame_rate.den;
			double actual_fps = rfps / (double)rfps_base;

			if (actual_fps > 31.0 &&
				actual_fps > avg_fps * 1.9 && actual_fps < avg_fps * 2.1)
			{
				if (verbose > 0)
				{
					fprintf(stderr,"\nAssuming interlaced. Halving frame rate.\n");
				}
				context->settings.source_video_frame_rate.num =
					3 * lround((rfps / (double)rfps_base) * 0.166666667);
			}
			else if ((actual_fps < 12 || actual_fps > 30) && avg_fps >= 12 && avg_fps <= 30)
			{
				context->settings.source_video_frame_rate.num =
					3 * lround(avg_fps * 0.333333333);
			}
			else if (actual_fps > 31)
			{
				context->settings.source_video_frame_rate.num = 30;
			}
			else if (actual_fps < 12)
			{
				context->settings.source_video_frame_rate.num = 12;
			}

			AddNewInputCodec(context, avcodec_find_decoder_by_name(context->settings.video_codec_name));

			// Add the stream map for the video
			if (context->num_stream_maps == 0)
			{
				AVStreamMap video_map;
				video_map.file_index = context->num_input_files - 1;
				video_map.stream_index = i;
				video_map.sync_file_index = context->num_input_files - 1;
				video_map.sync_stream_index = i;
				AddNewStreamMap(context, video_map);
			}
			else
			{
				if (verbose >= 0)
				{
					fprintf(stderr, "Skipped extra video track.\n");
				}
			}
			break;
		case CODEC_TYPE_SUBTITLE:
			AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
			if (context->settings.subtitle_stream_index != i)
			{
				st->discard = AVDISCARD_ALL;
				break;
			}
			AVStreamMap subtitle_map;
			subtitle_map.file_index = context->num_input_files - 1;
			subtitle_map.stream_index = i;
			subtitle_map.sync_file_index = context->num_input_files - 1;
			subtitle_map.sync_stream_index = i;
			AddNewStreamMap(context, subtitle_map);
			break;
		case CODEC_TYPE_DATA:
		case CODEC_TYPE_ATTACHMENT:
		case CODEC_TYPE_UNKNOWN:
			  AddNewInputCodec(context, avcodec_find_decoder_by_name(NULL));
			st->discard = AVDISCARD_ALL;
			break;
		default:
			abort();
		}
	}

    context->input_files_ts_offset[context->num_input_files - 1] = -timestamp;

    dump_format(ic, context->num_input_files - 1, filename, 0);

	return 0;
}

static int configureOutputFiles(STMTranscodeContext *context, char *filename)
{
    int use_video, use_audio;
    int input_has_video, input_has_audio, input_has_subtitle;
    AVFormatParameters params, *ap = &params;
    AVOutputFormat *file_oformat;
	
	if (context->settings.output_segment_length != 0)
	{
		context->settings.output_file_base = filename;
		context->settings.output_file_path =
			av_malloc(strlen(filename) + FilenameSuffixLength() + 1);
		UpdateFilePathForSegment(&context->settings);
	}
	else
	{
		context->settings.output_file_path = av_malloc(strlen(filename) + 1);
		strcpy(context->settings.output_file_path, filename);
	}
	
	AddNewOutputFile(context, avformat_alloc_context());
	AVFormatContext *oc = context->output_files[context->num_output_files - 1];
	
    if (!oc) {
        return -1;
    }

	file_oformat = av_guess_format(context->settings.output_file_format, context->settings.output_file_path, NULL);
	if (!file_oformat) {
		fprintf(stderr, "Unable to find a suitable output format for '%s'\n",
				context->settings.output_file_path);
		context->num_output_files--;
		av_free(context->output_files[context->num_output_files]);
		
		return -1;
	}

    oc->oformat = file_oformat;
    av_strlcpy(oc->filename, context->settings.output_file_path, sizeof(oc->filename));

	use_video = file_oformat->video_codec != CODEC_ID_NONE || context->settings.video_codec_name;
	use_audio = file_oformat->audio_codec != CODEC_ID_NONE || context->settings.audio_codec_name;

	/* disable if no corresponding type found and at least one
	   input file */
	if (context->num_input_files > 0) {
		check_audio_video_sub_inputs(context,
			&input_has_video, &input_has_audio, &input_has_subtitle);
		if (!input_has_video)
			use_video = 0;
		if (!input_has_audio)
			use_audio = 0;
	}
	
	if (strncmp(context->settings.audio_stream_specifier, "nil", 4) == 0) {
		use_audio = 0;
	}

	if (use_video)
	{
		if (new_video_stream(context, oc))
		{
			return -1;
		}
	}

	if (use_audio)
	{
		if (new_audio_stream(context, oc))
		{
			return -1;
		}
	}
	
	if (context->settings.subtitle_stream_index != -1)
	{
		if (new_subtitle_stream(context, oc))
		{
			return -1;
		}
	}

	oc->timestamp = 0;
	
	for(; context->metadata_count>0; context->metadata_count--){
		av_metadata_set(&oc->metadata, context->metadata[context->metadata_count-1].key,
									   context->metadata[context->metadata_count-1].value);
	}
	av_metadata_conv(oc, oc->oformat->metadata_conv, NULL);

    /* check filename in case of an image number is expected */
    if (oc->oformat->flags & AVFMT_NEEDNUMBER) {
        if (!av_filename_number_test(oc->filename)) {
            return -1;
        }
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        /* open the file */
        if (url_fopen(&oc->pb, context->settings.output_file_path, URL_WRONLY) < 0) {
            fprintf(stderr, "Could not open '%s'\n", context->settings.output_file_path);
            return -1;
        }
    }

    memset(ap, 0, sizeof(*ap));
    if (av_set_parameters(oc, ap) < 0) {
        fprintf(stderr, "%s: Invalid encoding parameters\n",
                oc->filename);
        return -1;
    }

    oc->preload= 0.5;
    oc->max_delay= context->settings.input_start_time;
    oc->loop_output = 0;
    oc->flags |= AVFMT_FLAG_NONBLOCK;
	
	return 0;
}

static int configureMappings(STMTranscodeContext *context)
{
	return 0;
}

static int applyInitialSettings(STMTranscodeSettings *settings,
	int64_t segment_duration, int64_t initial_segment,
	char *input_file_name)
{
	if (segment_duration == 0)
	{
		settings->subtitle_stream_index = -1;
		settings->output_single_frame_only = 1;
	}
	else
	{
		settings->subtitle_stream_index = -1;
		settings->output_segment_length = (double)segment_duration;
		settings->output_segment_index = initial_segment;
		settings->output_initial_segment = initial_segment;
		settings->input_start_time =
			(int64_t)(segment_duration * initial_segment * AV_TIME_BASE);
	}
	settings->input_file_name = input_file_name;

	return 0;
}

static char *newFileStringWithDifferentExtension(char *filename, char *ext)
{
	int i;
	int ext_length = strlen(ext);
	int filename_length = strlen(filename);
	char *other = av_malloc(filename_length + ext_length + 1 + 1);
	
	for (i = strlen(filename) - 1; i >= 0; i--)
	{
		if (filename[i] == '.')
		{
			break;
		}
	}
	if (i <= 0)
	{
		return NULL;
	}
	strncpy(other, filename, i);
	other[i] = '.';
	strncpy(&other[i+1], ext, ext_length);
	other[i + 1 + ext_length] = '\0';
	return other;
}

static char *newFileStringWithAdditionalExtension(char *filename, char *ext)
{
	char *other;
	asprintf(&other, "%s.%s", filename, ext);
	if (!other)
	{
		return 0;
	}

	return other;
}

static char *newLanguageFromStreamSpecifier(char *specifier, long *indexOut)
{
	char *language = av_malloc(strlen(specifier) + 1);
	char *languageCopy = language;
	while (*specifier != '-' && *specifier != '\0')
	{
		*languageCopy++ = *specifier++;
	}
	if (languageCopy == language)
	{
		av_free(language);
		return NULL;
	}
	*languageCopy = '\0';
	
	if (*specifier == '-')
	{
		specifier++;
	}
	
	char *outSpecifier;
	long languageIndex = strtol(specifier, &outSpecifier, 10);
	
	if (outSpecifier != specifier)
	{
		*indexOut = languageIndex;
	}
	else
	{
		*indexOut = -1;
	}
	return language;
}

static int fileExistsWithDifferentExtension(char *filename, char *ext)
{
	char *other = newFileStringWithDifferentExtension(filename, ext);
	if (!other)
	{
		return 0;
	}
	FILE *file = fopen(other, "r");
	int result = 0;
	if (file)
	{
		fclose(file);
		result = 1;
	}
	
	av_free(other);
	return result;
}

static int fileExistsWithAdditionalExtension(char *filename, char *ext)
{
	char *other;
	asprintf(&other, "%s.%s", filename, ext);
	if (!other)
	{
		return 0;
	}

	FILE *file = fopen(other, "r");
	int result = 0;
	if (file)
	{
		fclose(file);
		result = 1;
	}
	
	av_free(other);
	return result;
}

static int isParseableSubtitleCodec(AVCodecContext *codec)
{
	if (codec->codec_type != CODEC_TYPE_SUBTITLE)
	{
		return 0;
	}
	if (codec->codec_id == CODEC_ID_SSA)
	{
		return 1;
	}
	if (codec->codec_id == CODEC_ID_TEXT)
	{
		return 1;
	}
	if (codec->codec_id == CODEC_ID_MOV_TEXT && codec->codec_tag == MKTAG('t', 'x', '3', 'g'))
	{
		return 1;
	}
	return 0;
}

static int applyOutputSettings(STMTranscodeContext *context,
	char *stream_quality_name,
	char *audio_stream_specifier,
	char *subtitle_stream_specifier,
	float audio_gain,
	int album_art,
	char *srt_encoding,
	int flip_srt)
{
	if (context->input_files[0]->duration != AV_NOPTS_VALUE)
	{
		fprintf(stdout, "Duration: %g\n", context->input_files[0]->duration / (double)AV_TIME_BASE);
		fflush(stdout);
	}
	else
	{
		fprintf(stdout, "Duration: -1\n");
		fflush(stdout);
	}

	int videoStreamIndex = -1;
	for (int i = 0; i < context->input_files[0]->nb_streams; i++)
	{
		if (context->input_files[0]->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO)
		{
			videoStreamIndex = i;
			break;
		}
	}
	
	if (videoStreamIndex == -1 && album_art)
	{
		AVMetadata *metadata = context->input_files[0]->metadata;
		AVMetadataTag *tag =
			av_metadata_get(metadata, "covr", NULL, AV_METADATA_IGNORE_SUFFIX);
		if (!tag)
		{
			tag = av_metadata_get(metadata, "PIC", NULL, AV_METADATA_IGNORE_SUFFIX);
		}
		if (!tag)
		{
			tag = av_metadata_get(metadata, "APIC", NULL, AV_METADATA_IGNORE_SUFFIX);
		}
		if (tag)
		{
			context->settings.album_art_mode = 1;
			
			inmemory_buffer = av_malloc(tag->data_size);
			inmemory_buffer_size = tag->data_size;
			memcpy(inmemory_buffer, tag->data, tag->data_size);
			
			register_inmemory_protocol();

			char *filename = context->settings.input_file_name;

			if (strncmp(inmemory_buffer, "\x89PNG", 4)==0)
			{
				context->settings.input_file_name = "inmemory://file.png";
				if (configureInputFiles(context))
				{
					context->settings.album_art_mode = 0;
					context->settings.input_file_name = filename;
				}
				else
				{
					videoStreamIndex = 0;
				}
			}
			else if (strncmp(inmemory_buffer, "BM", 2)==0)
			{
				context->settings.input_file_name = "inmemory://file.bmp";
				if (configureInputFiles(context))
				{
					context->settings.album_art_mode = 0;
					context->settings.input_file_name = filename;
				}
				else
				{
					videoStreamIndex = 0;
				}
			}
			else
			{
				context->settings.input_file_name = "inmemory://file.jpg";
				if (configureInputFiles(context))
				{
					context->settings.album_art_mode = 0;
					context->settings.input_file_name = filename;
				}
				else
				{
					videoStreamIndex = 0;
				}
			}
		}
	}

	if (audio_gain >= 0)
	{
		audio_gain += 1.0;
	}
	else
	{
		audio_gain = 1.0 / (1.0 - audio_gain);
	}

	STMTranscodeSettings *settings = &context->settings;
	
	settings->output_dts_delta_threshold = 10;
	settings->output_duration = context->input_files[0]->duration;
	settings->output_file_format = "mpegts";

	settings->audio_codec_name = "libmp3lame";
	settings->audio_sample_rate = 48000;
	settings->audio_drift_threshold = 0.1;
	settings->audio_volume = 256.0f * audio_gain;
	settings->audio_stream_specifier = audio_stream_specifier;
	settings->audio_channels = 2;
	settings->audio_bitrate = 128 * 1000;
	
	settings->video_codec_name = "libx264";
	settings->video_level = 30;

	settings->video_frame_rate.num = settings->source_video_frame_rate.num;
	settings->video_frame_rate.den = 1;

	settings->video_me_range = 16;
	settings->video_me_method = "hex";
	settings->video_sc_threshold = 0;
	settings->video_qmin = 10;
	settings->video_qmax = 51;
	settings->video_qdiff = 4;
	settings->video_frame_refs = 2;
	settings->video_flags = "+loop";
	settings->video_flags2 = "-wpred-dct8x8";
	settings->video_cmp = "+chroma";
	settings->video_partitions = "+parti8x8+parti4x4+partp8x8+partb8x8";
	settings->video_crf = 24;
	settings->video_frame_width = 640;
	settings->video_frame_height = 480;
	settings->video_bufsize = 2048 * 1024;
	settings->video_minrate = 1152 * 1024;
	settings->video_maxrate = 1792 * 1024;
	settings->video_qcomp = 0.7;
	settings->video_subq = 5;

	double subtitle_scale_factor = 1.5;

	if (videoStreamIndex == -1 || context->settings.album_art_mode)
	{
		if (!context->settings.album_art_mode)
		{
			settings->video_codec_name = NULL;
		}

		settings->video_frame_rate.num = 3;
		
		if (strcmp(stream_quality_name, "high") == 0 ||
			strcmp(stream_quality_name, "veryhigh") == 0 ||
			strcmp(stream_quality_name, "midhigh") == 0)
		{
			settings->video_frame_width = 480;
			settings->video_frame_height = 480;
			settings->audio_bitrate = 256 * 1000;
			settings->video_crf = 27;
		}
		else if (strcmp(stream_quality_name, "mid") == 0)
		{
			settings->audio_bitrate = 160 * 1000;
			settings->video_frame_width = 320;
			settings->video_frame_height = 320;
			settings->video_crf = 31;
		}
		else if(strcmp(stream_quality_name, "midlow") == 0)
		{
			settings->audio_bitrate = 112 * 1000;
			settings->video_frame_width = 320;
			settings->video_frame_height = 320;
			settings->video_crf = 31;
		}
		else // low, verylow
		{
			settings->audio_bitrate = 42 * 1000;
			settings->video_frame_width = 192;
			settings->video_frame_height = 192;
			settings->video_crf = 34;
			settings->video_frame_rate.num = 5;
			settings->video_frame_rate.den = 2;
		}

		settings->video_frame_refs = 8;
		settings->video_qcomp = 0.9;
		settings->video_subq = 8;
		settings->video_me_method = "zero";
	}
	else if (strcmp(stream_quality_name, "verylow") == 0)
	{
		settings->audio_bitrate = 16 * 1000;
		settings->audio_channels = 1;

		settings->video_crf = 23;
		settings->video_me_method = "umh";
		settings->video_frame_rate.num = 5;
		settings->video_frame_rate.den = 2;
		settings->video_frame_width = 192;
		settings->video_frame_height = 128;
		settings->video_bufsize = 128 * 1024;
		settings->video_minrate = 32 * 1024;
		settings->video_maxrate = 64 * 1024;
		settings->video_qcomp = 0.0;
		settings->video_subq = 8;
		settings->video_frame_refs = 6;
		
		subtitle_scale_factor = 3.0;
	}
	else if (strcmp(stream_quality_name, "low") == 0)
	{
		settings->audio_bitrate = 24 * 1000;
		settings->audio_channels = 1;

		settings->video_crf = 26;
		settings->video_me_method = "umh";
		settings->video_frame_rate.num = 12;
		settings->video_frame_width = 192;
		settings->video_frame_height = 128;
		settings->video_bufsize = 256 * 1024;
		settings->video_minrate = 144 * 1024;
		settings->video_maxrate = 208 * 1024;
		settings->video_qcomp = 0.15;
		settings->video_subq = 8;
		settings->video_frame_refs = 8;
		
		subtitle_scale_factor = 3.0;
	}
	else if (strcmp(stream_quality_name, "midlow") == 0)
	{
		settings->audio_bitrate = 48 * 1000;
		settings->audio_channels = 1;

		settings->video_crf = 26;
		settings->video_me_method = "umh";
		settings->video_frame_rate.num = 15;
		settings->video_frame_width = 240;
		settings->video_frame_height = 160;
		settings->video_bufsize = 256 * 1024;
		settings->video_minrate = 144 * 1024;
		settings->video_maxrate = 208 * 1024;
		settings->video_qcomp = 0.225;
		settings->video_subq = 8;
		settings->video_frame_refs = 6;
		
		subtitle_scale_factor = 2.25;
	}
	else if (strcmp(stream_quality_name, "mid") == 0)
	{
		settings->audio_bitrate = 96 * 1000;
		settings->audio_channels = 2;

		settings->video_crf = 26;
		settings->video_me_method = "umh";
		settings->video_frame_width = 360;
		settings->video_frame_height = 240;
		settings->video_bufsize = 2048 * 1024;
		settings->video_minrate = 256 * 1024;
		settings->video_maxrate = 416 * 1024;
		settings->video_qcomp = 0.30;
		settings->video_frame_refs = 4;
		
		subtitle_scale_factor = 1.75;
	}
	else if (strcmp(stream_quality_name, "midhigh") == 0 ||
		settings->source_frame_height <= 320)
	{
		settings->video_crf = 26;
		settings->video_frame_width = 480;
		settings->video_frame_height = 320;
		settings->video_bufsize = 2048 * 1024;
		settings->video_minrate = 256 * 1024;
		settings->video_maxrate = 416 * 1024;
		settings->video_qcomp = 0.5;
		settings->video_frame_refs = 2;
	}
	else if (strcmp(stream_quality_name, "veryhigh") == 0 &&
		settings->source_frame_height > 480)
	{
		settings->video_crf = 27;
		settings->video_frame_width = 1024;
		settings->video_frame_height = 720;
		settings->video_frame_refs = 0;
		settings->video_subq = 1;
		settings->video_me_method = "dia";
	}
	settings->video_g =
		lround(GOP_LENGTH * settings->source_video_frame_rate.num /
			(double)settings->source_video_frame_rate.den);

	if (strncmp(audio_stream_specifier, "nil", 4) != 0)
	{
		long languageIndex;
		char *language = newLanguageFromStreamSpecifier(audio_stream_specifier, &languageIndex);
		long numMatchingStreams = 0;
		long firstIndex = -1;
		long firstLangIndex = -1;
		
		long i;
		for (i = 0; i < context->input_files[0]->nb_streams; i++)
		{
			AVStream *stream = context->input_files[0]->streams[i];
			if (stream->codec->codec_type != CODEC_TYPE_AUDIO)
			{
				continue;
			}
			if (firstIndex == -1)
			{
				firstIndex = i;
			}
			if (languageIndex == -1 && strncmp(language, "any", 4) != 0)
			{
				break;
			}
			AVMetadataTag *tag = av_metadata_get(stream->metadata, "language", NULL, 0);
			if (((!tag || !tag->value) && strcmp("und", language) == 0) ||
				(tag && tag->value && strcmp(tag->value, language) == 0))
			{
				if (firstLangIndex == -1)
				{
					firstLangIndex = i;
				}
				numMatchingStreams++;
				if (numMatchingStreams == languageIndex)
				{
					break;
				}
			}
		}
		
		if (i == context->input_files[0]->nb_streams)
		{
			if (firstLangIndex != -1)
			{
				i = firstLangIndex;
			}
			else if (firstIndex != -1)
			{
				i = firstIndex;
			}
		}
		
		if (i != context->input_files[0]->nb_streams)
		{
			AVStreamMap audio_map;
			audio_map.file_index = 0;
			audio_map.stream_index = i;
			audio_map.sync_file_index = 0;
			audio_map.sync_stream_index = i;
			AddNewStreamMap(context, audio_map);
		}
		
		av_free(language);
	}

	if (settings->album_art_mode)
	{
		asprintf(&context->settings.video_filters,
			"inlineass=pts_offset:%lld|font_scale:%f",
			(int64_t)(context->settings.output_segment_length * context->settings.output_initial_segment) * AV_TIME_BASE,
			subtitle_scale_factor);
	}
	else if (strncmp(subtitle_stream_specifier, "nil", 4) != 0)
	{
		long languageIndex;
		char *language = newLanguageFromStreamSpecifier(subtitle_stream_specifier, &languageIndex);
		long numMatchingStreams = 0;
		long firstIndex = -1;
		long firstLangIndex = -1;
		int ssa_exists = fileExistsWithDifferentExtension(context->settings.input_file_name, "ssa");
		int ssa2_exists = fileExistsWithAdditionalExtension(context->settings.input_file_name, "ssa");
		int ass_exists = fileExistsWithDifferentExtension(context->settings.input_file_name, "ass");
		int ass2_exists = fileExistsWithAdditionalExtension(context->settings.input_file_name, "ass");
		int srt_exists = fileExistsWithDifferentExtension(context->settings.input_file_name, "srt");
		int srt2_exists = fileExistsWithAdditionalExtension(context->settings.input_file_name, "srt");
		
		if (languageIndex == -1 && ssa_exists &&
			(strncmp(language, "ssa", 4) == 0 || !srt_exists))
		{
			subtitle_path = newFileStringWithDifferentExtension(context->settings.input_file_name, "ssa");
			asprintf(&settings->video_filters,
				"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
				(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
				subtitle_scale_factor,
					srt_encoding,
					flip_srt);
		}
		else if (languageIndex == -1 && ssa2_exists &&
			(strncmp(language, "ssa", 4) == 0 || !(srt_exists || srt2_exists)))
		{
			subtitle_path = newFileStringWithAdditionalExtension(context->settings.input_file_name, "ssa");
			asprintf(&settings->video_filters,
				"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
				(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
				subtitle_scale_factor,
					srt_encoding,
					flip_srt);
		}
		else if (languageIndex == -1 && ass_exists &&
			(strncmp(language, "ass", 4) == 0 || !srt_exists))
		{
			subtitle_path = newFileStringWithDifferentExtension(context->settings.input_file_name, "ass");
			asprintf(&settings->video_filters,
				"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
				(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
				subtitle_scale_factor,
					srt_encoding,
					flip_srt);
		}
		else if (languageIndex == -1 && ass2_exists &&
			(strncmp(language, "ass", 4) == 0 || !(srt_exists || srt2_exists)))
		{
			subtitle_path = newFileStringWithAdditionalExtension(context->settings.input_file_name, "ass");
			asprintf(&settings->video_filters,
				"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
				(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
				subtitle_scale_factor,
					srt_encoding,
					flip_srt);
		}
		else if (languageIndex == -1 && srt_exists)
		{
			subtitle_path = newFileStringWithDifferentExtension(context->settings.input_file_name, "srt");
			asprintf(&settings->video_filters,
				"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
				(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
				subtitle_scale_factor,
					srt_encoding,
					flip_srt);
		}
		else if (languageIndex == -1 && srt2_exists)
		{
			subtitle_path = newFileStringWithAdditionalExtension(context->settings.input_file_name, "srt");
			asprintf(&settings->video_filters,
				"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
				(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
				subtitle_scale_factor,
					srt_encoding,
					flip_srt);
		}
		else
		{
			long i;
			for (i = 0; i < context->input_files[0]->nb_streams; i++)
			{
				AVStream *stream = context->input_files[0]->streams[i];

				if (!isParseableSubtitleCodec(stream->codec))
				{
					continue;
				}
				if (firstIndex == -1)
				{
					firstIndex = i;
				}
				if (languageIndex == -1 && strncmp(language, "any", 4) != 0)
				{
					break;
				}
				AVMetadataTag *tag = av_metadata_get(stream->metadata, "language", NULL, 0);
				if (((!tag || !tag->value) && strcmp("und", language) == 0) ||
					(tag && tag->value && strcmp(tag->value, language) == 0))
				{
					if (firstLangIndex == -1)
					{
						firstLangIndex = i;
					}
					numMatchingStreams++;
					if (numMatchingStreams == languageIndex)
					{
						break;
					}
				}
			}
			
			if (i == context->input_files[0]->nb_streams)
			{
				if (firstLangIndex != -1)
				{
					i = firstLangIndex;
				}
				else if (firstIndex != -1)
				{
					i = firstIndex;
				}
			}

			if (i != context->input_files[0]->nb_streams)
			{
				asprintf(&settings->video_filters,
					"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
					(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
					subtitle_scale_factor,
					srt_encoding,
					flip_srt);
				context->settings.subtitle_stream_index = i;
				
				configureInputFiles(context);
			}
			else if (ssa_exists)
			{
				char *subtitle_file_name = newFileStringWithDifferentExtension(context->settings.input_file_name, "ssa");
				asprintf(&settings->video_filters,
					"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
					(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
					subtitle_scale_factor,
					srt_encoding,
					flip_srt);
				av_free(subtitle_file_name);
			}
			else if (srt_exists)
			{
				char *subtitle_file_name = newFileStringWithDifferentExtension(context->settings.input_file_name, "srt");
				asprintf(&settings->video_filters,
					"inlineass=pts_offset:%lld|font_scale:%f|srt_encoding:%s|flip_srt:%d",
					(int64_t)(settings->output_segment_length * settings->output_initial_segment) * AV_TIME_BASE,
					subtitle_scale_factor,
					srt_encoding,
					flip_srt);
				av_free(subtitle_file_name);
			}
		}
		av_free(language);
	}

	return 0;
}

static char *escapeQuotes(char *str)
{
	int quoteCount = 0;
	int length = strlen(str);
	int i;
	for (i = 0; i < length; i++)
	{
		if (str[i] == '"')
		{
			quoteCount++;
		}
	}
	if (!quoteCount)
	{
		return str;
	}
	char *result = av_malloc(length + quoteCount + 1);
	int destOffset = 0;
	for (i = 0; i < length; i++)
	{
		result[i + destOffset] = str[i];
		if (result[i + destOffset] == '"')
		{
			result[i + destOffset] = '\\';
			destOffset++;
			result[i + destOffset] = '"';
		}
	}
	result[i + destOffset] = '\0';
	av_free(str);

	return result;
}

static int writeJsonToFile(STMTranscodeContext *context, FILE *file)
{
	int isDirname;
	int isFilename;
	char *artist = escapeQuotes(newArtistForFile(context->input_files[0], &isDirname));
	char *title = escapeQuotes(newTitleForFile(context->input_files[0], &isFilename));

	int jsonLength = 0;

	jsonLength += fprintf(file,"{");
	
	if (!isDirname)
	{
		jsonLength += fprintf(file,"\"artist\":\"%s\",", artist);
	}
	if (!isFilename)
	{
		jsonLength += fprintf(file,"\"title\":\"%s\",", title);
	}

	av_free(artist);
	av_free(title);

	jsonLength += fprintf(file,"\"length\":%f,\"audio_streams\":[",
		(double)context->input_files[0]->duration /
		(double)AV_TIME_BASE);

	int audio_track_count = 0;
	for (int i = 0; i < context->input_files[0]->nb_streams; i++)
	{
		if (context->input_files[0]->streams[i]->codec->codec_type == CODEC_TYPE_AUDIO)
		{
			if (audio_track_count != 0)
			{
				jsonLength += fprintf(file,",");
			}
			audio_track_count++;

			AVMetadataTag *languageTag =
				av_metadata_get(context->input_files[0]->streams[i]->metadata, "language", NULL, 0);
			AVMetadataTag *titleTag =
				av_metadata_get(context->input_files[0]->streams[i]->metadata, "title", NULL, 0);
			if (titleTag && titleTag->value)
			{
				jsonLength += fprintf(file,"{\"language\":\"%s\",\"title\":\"%s\"}",
					(languageTag && languageTag->value) ? languageTag->value : "und",
					titleTag->value);
			}
			else
			{
				jsonLength += fprintf(file,"{\"language\":\"%s\"}", (languageTag && languageTag->value) ? languageTag->value : "und");
			}
		}
	}
	jsonLength += fprintf(file,"],\"subtitle_streams\":[");
	int subtitle_track_count = 0;
	for (int i = 0; i < context->input_files[0]->nb_streams; i++)
	{
		AVStream *stream = context->input_files[0]->streams[i];
		if (!isParseableSubtitleCodec(stream->codec))
		{
			continue;
		}
		if (subtitle_track_count != 0)
		{
			jsonLength += fprintf(file,",");
		}
		subtitle_track_count++;

		AVMetadataTag *languageTag =
			av_metadata_get(stream->metadata, "language", NULL, 0);
		AVMetadataTag *titleTag =
			av_metadata_get(stream->metadata, "title", NULL, 0);
		if (titleTag && titleTag->value)
		{
			jsonLength += fprintf(file,"{\"language\":\"%s\",\"title\":\"%s\"}",
				(languageTag && languageTag->value) ? languageTag->value : "und",
				titleTag->value);
		}
		else
		{
			jsonLength += fprintf(file,"{\"language\":\"%s\"}", (languageTag && languageTag->value) ? languageTag->value : "und");
		}
	}
	if (fileExistsWithDifferentExtension(context->settings.input_file_name, "ssa") ||
		fileExistsWithAdditionalExtension(context->settings.input_file_name, "ssa") ||
		fileExistsWithDifferentExtension(context->settings.input_file_name, "ass") ||
		fileExistsWithAdditionalExtension(context->settings.input_file_name, "ass"))
	{
		if (subtitle_track_count != 0)
		{
			jsonLength += fprintf(file,",");
		}
		subtitle_track_count++;
		jsonLength += fprintf(file,"{\"file\":\"ssa\"}");
	}
	if (fileExistsWithDifferentExtension(context->settings.input_file_name, "srt") ||
		fileExistsWithAdditionalExtension(context->settings.input_file_name, "srt"))
	{
		if (subtitle_track_count != 0)
		{
			jsonLength += fprintf(file,",");
		}
		subtitle_track_count++;
		jsonLength += fprintf(file,"{\"file\":\"srt\"}");
	}

	int videoStreamIndex = -1;
	for (int i = 0; i < context->input_files[0]->nb_streams; i++)
	{
		if (context->input_files[0]->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO)
		{
			videoStreamIndex = i;
			break;
		}
	}
	if (videoStreamIndex == -1)
	{
		jsonLength += fprintf(file,"],\"has_video\":false}");
	}
	else
	{
		jsonLength += fprintf(file,"],\"has_video\":true}");
	}
	
	return jsonLength;
}

static int applyMetadataOutputSettings(STMTranscodeContext *context)
{
	int videoStreamIndex = -1;
	for (int i = 0; i < context->input_files[0]->nb_streams; i++)
	{
		if (context->input_files[0]->streams[i]->codec->codec_type == CODEC_TYPE_VIDEO)
		{
			videoStreamIndex = i;
			break;
		}
	}
	
	int jsonLength = writeJsonToFile(context, stderr);
	fprintf(stderr, "jsonLength is %d\n", jsonLength);
	fwrite(&jsonLength, sizeof(int), 1, stdout);
	writeJsonToFile(context, stdout);
	fflush(stdout);
	
	if (videoStreamIndex == -1)
	{
		AVMetadata *metadata = context->input_files[0]->metadata;
		AVMetadataTag *tag =
			av_metadata_get(metadata, "covr", NULL, AV_METADATA_IGNORE_SUFFIX);
		if (!tag)
		{
			tag = av_metadata_get(metadata, "PIC", NULL, AV_METADATA_IGNORE_SUFFIX);
		}
		if (!tag)
		{
			tag = av_metadata_get(metadata, "APIC", NULL, AV_METADATA_IGNORE_SUFFIX);
		}
		if (tag)
		{
			inmemory_buffer = av_malloc(tag->data_size);
			inmemory_buffer_size = tag->data_size;
			memcpy(inmemory_buffer, tag->data, tag->data_size);
			
			register_inmemory_protocol();

			av_close_input_file(context->input_files[0]);
			context->input_files[0] = NULL;
			context->num_input_files--;

			memset(context, 0, sizeof(STMTranscodeContext));
			
			context->settings.output_single_frame_only = 1;
			context->settings.subtitle_stream_index = -1;
			
			if (strncmp(inmemory_buffer, "\x89PNG", 4)==0)
			{
				context->settings.input_file_name = "inmemory://file.png";
				if (configureInputFiles(context))
				{
					return 1;
				}
			}
			else if (strncmp(inmemory_buffer, "BM", 2)==0)
			{
				context->settings.input_file_name = "inmemory://file.bmp";
				if (configureInputFiles(context))
				{
					return 1;
				}
			}
			else
			{
				context->settings.input_file_name = "inmemory://file.jpg";
				if (configureInputFiles(context))
				{
					return 1;
				}
			}
			
			videoStreamIndex = 0;
		}
		else
		{
			fprintf(stderr, "No video and no covr tag.");
			return 1;
		}
	}
			
	STMTranscodeSettings *settings = &context->settings;

	settings->audio_stream_specifier = "nil";
	settings->subtitle_stream_specifier = "nil";

	settings->input_start_time = 0.7;
	settings->output_dts_delta_threshold = 10;
	settings->output_duration = INT64_MAX;

	settings->output_file_format = "image2pipe";
	settings->video_codec_name = "mjpeg";
	
	const double target_width = 108;
	const double target_height = 72;
	
	AVStream *stream = context->input_files[0]->streams[videoStreamIndex];
	double stream_width = stream->codec->width;
	double stream_height = stream->codec->height;
	
	if (stream->codec->sample_aspect_ratio.num > 0 && stream->codec->sample_aspect_ratio.den > 0)
	{
		stream_width *= stream->codec->sample_aspect_ratio.num / (double)stream->codec->sample_aspect_ratio.den;
	}
	
	if (target_width / target_height >
		stream_width / stream_height)
	{
		settings->video_frame_height = target_height;
		settings->video_frame_width = target_height * stream_width / stream_height;
	}
	else
	{
		settings->video_frame_width = target_width;
		settings->video_frame_height = target_width * stream_height / stream_width;
	}

	settings->video_qmin = 2;
	settings->video_qmax = 31;
	settings->video_qcomp = 0.5;
	settings->video_qdiff = 3;
	settings->video_g = 12;

	return 0;
}

enum
{
	PROCESS_NAME,
	OPERATION_NAME,
	INPUT_PATH,
	OUTPUT_FILE_PATH_PREFIX,
	VARIANT_NAME,
	SEGMENT_DURATION,
	INITIAL_SEGMENT_INDEX,
	AUDIO_STREAM_SPECIFIER,
	SUBTITLE_STREAM_SPECIFIER,
	AUDIO_GAIN,
	ALBUM_ART,
	SRT_ENCODING,
	SRT_DIRECTION,
	NUM_ARGUMENTS
};

int main(int argc, char **argv)
{
	int result = 1;
	
#ifndef WINDOWS
#ifndef LINUX
	pthread_t thread; 
	pthread_create (&thread, 0, WatchForParentTermination, 0);
#endif
#endif

	signal(SIGABRT, SignalHandler);
	signal(SIGILL, SignalHandler);
	signal(SIGSEGV, SignalHandler);
	signal(SIGFPE, SignalHandler);

	executable_path = argv[0];
	subtitle_path = NULL;

#ifdef WINDOWS
	char fetchedPath[MAX_PATH+1];
	if (strlen(executable_path) == 0)
	{
		GetModuleFileName(NULL, (TCHAR *)fetchedPath, MAX_PATH+1);
		executable_path = fetchedPath;
	}
#endif

	if (argc > OPERATION_NAME &&
		strcmp(argv[OPERATION_NAME], "fontcache") == 0)
	{
		char *conf_path;
#ifdef WINDOWS
		char *lastSlash = strrchr(executable_path, '\\');
		int length = (long)lastSlash - (long)executable_path;
		asprintf(&conf_path, "%.*s\\%s", length, executable_path, "fonts-windows.conf");
#else
		char *lastSlash = strrchr(executable_path, '/');
		int length = (long)lastSlash - (long)executable_path;
		asprintf(&conf_path, "%.*s/%s", length, executable_path, "fonts.conf");
#endif
	
		FcConfig *config = FcConfigCreate();
		FcConfigParseAndLoad(config, (const FcChar8 *)conf_path, FcTrue);
		FcConfigBuildFonts(config);

		av_free(conf_path);
		return 0;
	}
	
	if (argc < INPUT_PATH + 1)
	{
		return 1;
	}
	
	STMTranscodeContext transcode_context;
	memset(&transcode_context, 0, sizeof(STMTranscodeContext));

    avcodec_register_all();
    avfilter_register_all();
    av_register_all();

	if (argc > INPUT_PATH &&
		strcmp(argv[OPERATION_NAME], "metadata") == 0)
	{
		applyInitialSettings(&transcode_context.settings, 0, 0, argv[INPUT_PATH]);
		
		/* Set the input file */
		if (configureInputFiles(&transcode_context))
		{
			goto close_and_exit;
		}

		if (applyMetadataOutputSettings(&transcode_context))
		{
			goto close_and_exit;
		}

		/* Set the output file */
		if (configureOutputFiles(
			&transcode_context,
			"pipe:"))
		{
			goto close_and_exit;
		}
		
		/* Configure the mappings */
		if (configureMappings(&transcode_context))
		{
			goto close_and_exit;
		}
	}
	else if (argc == NUM_ARGUMENTS &&
		strcmp(argv[OPERATION_NAME], "transcode") == 0)
	{
		/* Set the initial settings */
		if (applyInitialSettings(
			&transcode_context.settings,
			strtoll(argv[SEGMENT_DURATION], NULL, 10),
			strtoll(argv[INITIAL_SEGMENT_INDEX], NULL, 10),
			argv[INPUT_PATH]))
		{
			goto close_and_exit;
		}
		
		/* Set the input file */
		if (configureInputFiles(&transcode_context))
		{
			goto close_and_exit;
		}

		if (applyOutputSettings(
			&transcode_context,
			argv[VARIANT_NAME],
			argv[AUDIO_STREAM_SPECIFIER],
			argv[SUBTITLE_STREAM_SPECIFIER],
			strtof(argv[AUDIO_GAIN], NULL),
			strcmp(argv[ALBUM_ART], "yes") == 0,
			argv[SRT_ENCODING],
			strcmp(argv[SRT_DIRECTION], "rtl") == 0))
		{
			goto close_and_exit;
		}

		/* Set the output file */
		if (configureOutputFiles(
			&transcode_context,
			argv[OUTPUT_FILE_PATH_PREFIX]))
		{
			goto close_and_exit;
		}
		
		/* Configure the mappings */
		if (configureMappings(&transcode_context))
		{
			goto close_and_exit;
		}
	}
	else
	{
		goto close_and_exit;
	}
	
	if(av_encode(&transcode_context))
	{
		goto close_and_exit;
	}
	
	result = 0;
	if (verbose > 0)
	{
	    fprintf(stderr, "Encode finished successfully.\n");
	}

	goto close_and_exit;
	
close_and_exit:
	av_close(&transcode_context);
	
	if (result)
	{
    	fprintf(stderr, "Exiting after failure.\n");
	}
	
	return result;
}
