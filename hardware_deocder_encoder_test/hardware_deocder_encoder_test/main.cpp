#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/display.h"
#include "libavutil/eval.h"
#include "libavutil/base64.h"
#include "libavutil/error.h"
#include "libavutil/opt.h"
#include "libavutil/version.h"
#include "libswresample/swresample.h"
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include "libavutil/imgutils.h"
#include "SDL.h"
}

#if 1

//hardware encoder
#define HARDWARE_ENCODER_CUVID 0
#define INPUT_RGBA_FILE 1
#define OUTPUT_MP4_FILE 0

int main(int argc, char* argv[])
{
	AVCodec *pCodec = NULL;
    AVCodecContext *pCodecCtx= NULL;
    FILE *fp_in = NULL;
	FILE *fp_out = NULL;
	FILE *fp_yuv = NULL;
	AVFrame *pFrame = NULL;
    AVPacket pkt;
	int y_size;
	int framecnt=0;
	int i, ret, got_output;

	AVCodecID codec_id = AV_CODEC_ID_H264;

	AVFrame *pRgbFrame = NULL;
	struct SwsContext *img_convert_ctx = NULL;
#if INPUT_RGBA_FILE
	char filename_in[]="1920x1080.rgb";
#else
	char filename_in[]="111.yuv";
#endif

#if HARDWARE_ENCODER_CUVID
	char filename_out[]="ds-cuda.h264";
#else
	char filename_out[] = "ds.h264";
#endif

#if INPUT_RGBA_FILE
	int in_w=1920,in_h=1080;	
	int framenum = 1000;
#else
	int in_w = 1920, in_h = 1080;
	int framenum = 1000;
#endif

	avcodec_register_all();

#if OUTPUT_MP4_FILE
	char out_format[] = "mp4";
	char out_path[] = "C:/out.mp4";
	AVOutputFormat *output_format = av_guess_format(out_format, out_path, NULL);

	AVFormatContext    *output = NULL;
	avformat_alloc_output_context2(&output, output_format, NULL, NULL);
	output->oformat->video_codec = AV_CODEC_ID_H264;
#endif

#if HARDWARE_ENCODER_CUVID
	pCodec = avcodec_find_encoder_by_name("h264_nvenc");
#else
	pCodec = avcodec_find_encoder(codec_id);
#endif
    if (!pCodec) {
        printf("Codec not found\n");
        return -1;
    }

#if OUTPUT_MP4_FILE
	AVStream           *video = NULL;
	video = avformat_new_stream(output, (const AVCodec *)pCodec);
	video->id = output->nb_streams - 1;
#endif

    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        printf("Could not allocate video codec context\n");
        return -1;
    }
    pCodecCtx->bit_rate = 8*1024*1024;
    pCodecCtx->width = in_w;
    pCodecCtx->height = in_h;
    pCodecCtx->time_base.num=1;
	pCodecCtx->time_base.den=30;
    pCodecCtx->gop_size = 5;
    pCodecCtx->max_b_frames = 0;
    pCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (codec_id == AV_CODEC_ID_H264)
        av_opt_set(pCodecCtx->priv_data, "preset", "slow", 0);
 
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        printf("Could not open codec\n");
        return -1;
    }
    
    pFrame = av_frame_alloc();
    if (!pFrame) {
        printf("Could not allocate video frame\n");
        return -1;
    }
    pFrame->format = pCodecCtx->pix_fmt;
    pFrame->width  = pCodecCtx->width;
    pFrame->height = pCodecCtx->height;

    ret = av_image_alloc(pFrame->data, pFrame->linesize, pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, 16);
    if (ret < 0) {
        printf("Could not allocate raw picture buffer\n");
        return -1;
    }

#if INPUT_RGBA_FILE
	pRgbFrame = av_frame_alloc();
	if (!pRgbFrame) {
		printf("Could not allocate rgb frame\n");
		return -1;
	}
	pRgbFrame->format = AV_PIX_FMT_RGBA;
	pRgbFrame->width = pCodecCtx->width;
	pRgbFrame->height = pCodecCtx->height;

	const int pixelSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, pCodecCtx->width, pCodecCtx->height, 1);
	pRgbFrame->data[0] = (unsigned char*)calloc(1, pixelSize);
	pRgbFrame->linesize[0] = 4 * pCodecCtx->width;

	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGBA, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
#endif

#if OUTPUT_MP4_FILE
	if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
		ret = avio_open(&output->pb, out_path, AVIO_FLAG_WRITE);
		if (ret < 0) {
			printf("avio_open error.\n");
			return -1;
		}
	}

	strncpy(output->filename, out_path, sizeof(output->filename));
	output->filename[sizeof(output->filename) - 1] = 0;

	ret = avformat_write_header(output, NULL);
	if (ret < 0) {
		printf("writer header error\n");
		return -1;
	}
#endif

	double start = GetTickCount();

	//Input raw data
	fp_in = fopen(filename_in, "rb+");
	if (!fp_in) {
		printf("Could not open %s\n", filename_in);
		return -1;
	}

	//Output bitstream
	fp_out = fopen(filename_out, "wb+");
	if (!fp_out) {
		printf("Could not open %s\n", filename_out);
		return -1;
	}
	fp_yuv = fopen("yuv_out.yuv", "wb+");
	if (!fp_yuv) {
		printf("Could not open %s\n", filename_out);
		return -1;
	}

	y_size = pCodecCtx->width * pCodecCtx->height;
    for (i = 0; i < framenum; i++) {
        av_init_packet(&pkt);
        pkt.data = NULL;    // packet data will be allocated by the encoder
        pkt.size = 0;

#if INPUT_RGBA_FILE
		//Read raw rgb data
		int ret = fread(pRgbFrame->data[0], 1, pixelSize, fp_in);
		if ( ret<= 0){
			break;
		} else if(feof(fp_in)){
			break;
		}
		sws_scale(img_convert_ctx, (const uint8_t* const*)pRgbFrame->data, pRgbFrame->linesize, 0,
			pCodecCtx->height, pFrame->data, pFrame->linesize);

		fwrite(pFrame->data[0], 1, y_size, fp_yuv);      //Y 
		fwrite(pFrame->data[1], 1, y_size / 4, fp_yuv);  //U
		fwrite(pFrame->data[2], 1, y_size / 4, fp_yuv);  //V
		fflush(fp_yuv);
#else
		//Read raw YUV data
		if (fread(pFrame->data[0],1,y_size,fp_in)<= 0||		// Y
			fread(pFrame->data[1],1,y_size/4,fp_in)<= 0||	// U
			fread(pFrame->data[2],1,y_size/4,fp_in)<= 0){	// V
			return -1;
		}else if(feof(fp_in)){
			break;
		}
#endif

   //     pFrame->pts = i;
   //     ret = avcodec_encode_video2(pCodecCtx, &pkt, pFrame, &got_output);
   //     if (ret < 0) {
   //         printf("Error encoding frame\n");
   //         return -1;
   //     }
   //     if (got_output) {
   //         printf("Succeed to encode frame: %5d\tsize:%5d\n",framecnt,pkt.size);
			//framecnt++;
   //         fwrite(pkt.data, 1, pkt.size, fp_out);
#if OUTPUT_MP4_FILE
			ret = av_interleaved_write_frame(output, &pkt);
			if (ret < 0) {
				printf("writer frame error\n");
			}
#endif
            //av_free_packet(&pkt);
       // }
    }

	double end = GetTickCount();
	printf("time cost: %fms", end - start);

    //Flush Encoder
    //for (got_output = 1; got_output; i++) {
    //    ret = avcodec_encode_video2(pCodecCtx, &pkt, NULL, &got_output);
    //    if (ret < 0) {
    //        printf("Error encoding frame\n");
    //        return -1;
    //    }
    //    if (got_output) {
    //        printf("Flush Encoder: Succeed to encode 1 frame!\tsize:%5d\n",pkt.size);
    //        fwrite(pkt.data, 1, pkt.size, fp_out);
    //        av_free_packet(&pkt);
    //    }
    //}

#if OUTPUT_MP4_FILE
	ret = av_write_trailer(output);
	if (ret != 0) {
		printf("writer tail error\n");
	}
#endif
	if (fp_in)
		fclose(fp_in);
	if (fp_in)
		fclose(fp_out);
	if (fp_in)
		fclose(fp_yuv);

	if (img_convert_ctx)
		sws_freeContext(img_convert_ctx);
	if (pCodecCtx){
		avcodec_close(pCodecCtx);
		av_free(pCodecCtx);
	}
	if (pRgbFrame){
		free(pRgbFrame->data[0]);
		av_frame_free(&pRgbFrame);
	}
    av_freep(&pFrame->data[0]);
    av_frame_free(&pFrame);

	system("pause");
	return 0;
}
#else
static AVCodecContext *c = NULL;
static AVFrame *frame;
static AVPacket pkt;
static FILE *file;
struct SwsContext *sws_context = NULL;

static void ffmpeg_encoder_set_frame_yuv_from_rgb(uint8_t *rgb) {
	const int in_linesize[1] = { 3 * c->width };
	sws_context = sws_getCachedContext(sws_context,
		c->width, c->height, AV_PIX_FMT_RGB24,
		c->width, c->height, AV_PIX_FMT_YUV420P,
		0, 0, 0, 0);
	sws_scale(sws_context, (const uint8_t * const *)&rgb, in_linesize, 0,
		c->height, frame->data, frame->linesize);
}

uint8_t* generate_rgb(int width, int height, int pts, uint8_t *rgb) {
	int x, y, cur;
	rgb = (uint8_t*)realloc(rgb, 3 * sizeof(uint8_t) * height * width);
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			cur = 3 * (y * width + x);
			rgb[cur + 0] = 0;
			rgb[cur + 1] = 0;
			rgb[cur + 2] = 0;
			if ((frame->pts / 25) % 2 == 0) {
				if (y < height / 2) {
					if (x < width / 2) {
						/* Black. */
					}
					else {
						rgb[cur + 0] = 255;
					}
				}
				else {
					if (x < width / 2) {
						rgb[cur + 1] = 255;
					}
					else {
						rgb[cur + 2] = 255;
					}
				}
			}
			else {
				if (y < height / 2) {
					rgb[cur + 0] = 255;
					if (x < width / 2) {
						rgb[cur + 1] = 255;
					}
					else {
						rgb[cur + 2] = 255;
					}
				}
				else {
					if (x < width / 2) {
						rgb[cur + 1] = 255;
						rgb[cur + 2] = 255;
					}
					else {
						rgb[cur + 0] = 255;
						rgb[cur + 1] = 255;
						rgb[cur + 2] = 255;
					}
				}
			}
		}
	}
	return rgb;
}

/* Allocate resources and write header data to the output file. */
void ffmpeg_encoder_start(const char *filename, int codec_id, int fps, int width, int height) {
	AVCodec *codec;
	int ret;

	codec = avcodec_find_encoder((AVCodecID)codec_id);
	if (!codec) {
		fprintf(stderr, "Codec not found\n");
		exit(1);
	}
	c = avcodec_alloc_context3(codec);
	if (!c) {
		fprintf(stderr, "Could not allocate video codec context\n");
		exit(1);
	}
	c->bit_rate = 400000;
	c->width = width;
	c->height = height;
	c->time_base.num = 1;
	c->time_base.den = fps;
	c->gop_size = 10;
	c->max_b_frames = 1;
	c->pix_fmt = AV_PIX_FMT_YUV420P;
	if (codec_id == AV_CODEC_ID_H264)
		av_opt_set(c->priv_data, "preset", "slow", 0);
	if (avcodec_open2(c, codec, NULL) < 0) {
		fprintf(stderr, "Could not open codec\n");
		exit(1);
	}
	file = fopen(filename, "wb");
	if (!file) {
		fprintf(stderr, "Could not open %s\n", filename);
		exit(1);
	}
	frame = av_frame_alloc();
	if (!frame) {
		fprintf(stderr, "Could not allocate video frame\n");
		exit(1);
	}
	frame->format = c->pix_fmt;
	frame->width = c->width;
	frame->height = c->height;
	ret = av_image_alloc(frame->data, frame->linesize, c->width, c->height, c->pix_fmt, 32);
	if (ret < 0) {
		fprintf(stderr, "Could not allocate raw picture buffer\n");
		exit(1);
	}
}

/*
Write trailing data to the output file
and free resources allocated by ffmpeg_encoder_start.
*/
void ffmpeg_encoder_finish(void) {
	uint8_t endcode[] = { 0, 0, 1, 0xb7 };
	int got_output, ret;
	do {
		fflush(stdout);
		ret = avcodec_encode_video2(c, &pkt, NULL, &got_output);
		if (ret < 0) {
			fprintf(stderr, "Error encoding frame\n");
			exit(1);
		}
		if (got_output) {
			fwrite(pkt.data, 1, pkt.size, file);
			av_packet_unref(&pkt);
		}
	} while (got_output);
	fwrite(endcode, 1, sizeof(endcode), file);
	fclose(file);
	avcodec_close(c);
	av_free(c);
	av_freep(&frame->data[0]);
	av_frame_free(&frame);
}

/*
Encode one frame from an RGB24 input and save it to the output file.
Must be called after ffmpeg_encoder_start, and ffmpeg_encoder_finish
must be called after the last call to this function.
*/
void ffmpeg_encoder_encode_frame(uint8_t *rgb) {
	int ret, got_output;
	ffmpeg_encoder_set_frame_yuv_from_rgb(rgb);
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;
	ret = avcodec_encode_video2(c, &pkt, frame, &got_output);
	if (ret < 0) {
		fprintf(stderr, "Error encoding frame\n");
		exit(1);
	}
	if (got_output) {
		fwrite(pkt.data, 1, pkt.size, file);
		av_packet_unref(&pkt);
	}
}

/* Represents the main loop of an application which generates one frame per loop. */
static void encode_example(const char *filename, int codec_id) {
	int pts;
	int width = 320;
	int height = 240;
	uint8_t *rgb = NULL;
	ffmpeg_encoder_start(filename, codec_id, 25, width, height);
	for (pts = 0; pts < 100; pts++) {
		frame->pts = pts;
		rgb = generate_rgb(width, height, pts, rgb);
		ffmpeg_encoder_encode_frame(rgb);
	}
	ffmpeg_encoder_finish();
	free(rgb);
}

int main(int argc, char* argv[]) {
	avcodec_register_all();
	encode_example("tmp.h264", AV_CODEC_ID_H264);
	//encode_example("tmp.mpg", AV_CODEC_ID_MPEG1VIDEO);
	/* TODO: is this encoded correctly? Possible to view it without container? */
	/*encode_example("tmp.vp8", AV_CODEC_ID_VP8);*/
	return 0;
}
#endif
