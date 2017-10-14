#include <stdio.h>
#include <stdlib.h>

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
#include "SDL.h"
}

#include "ffmpeg_dxva2.h"

//Output YUV420P 
#define OUTPUT_YUV420P 0

//hardware decoder
#define HARDWARE_DECODER_CUVID 1
#define HARDWARE_DECODER_DXVA2 0

#if HARDWARE_DECODER_CUVID
enum AVPixelFormat get_hwaccel_format(struct AVCodecContext *s, const enum AVPixelFormat * fmt)
{
	(void)s;
	(void)fmt;

	// for now force output to common denominator
	return AV_PIX_FMT_YUV420P;
}
#elif HARDWARE_DECODER_DXVA2
AVPixelFormat get_hwaccek_format(AVCodecContext *s, const AVPixelFormat *pix_fmts)
{
	InputStream* ist = (InputStream*)s->opaque;
	ist->active_hwaccel_id = HWACCEL_DXVA2;
	ist->hwaccel_pix_fmt = AV_PIX_FMT_DXVA2_VLD;
	return ist->hwaccel_pix_fmt;
}
#else
	//Nothing to do
#endif

static void nv12_to_yuv420(AVCodecContext *s, AVFrame *src, AVFrame *dst)
{
	//data
	int y_size = src->height * src->width;
	memcpy(dst->data[0], src->data[0], y_size);		//Y
	for (int i = 0; i < y_size / 4; i++){
		dst->data[2][i] = src->data[1][2 * i + 1];	//V
		dst->data[1][i] = src->data[1][2 * i + 0];	//U
	}

	//linesize
	dst->linesize[0] = src->linesize[0];
	dst->linesize[1] = src->linesize[1] / 2;
	dst->linesize[2] = dst->linesize[1];

	//other info
	dst->width = src->width;
	dst->height = src->height;
	dst->format = AV_PIX_FMT_YUV420P;
	dst->key_frame = src->key_frame;
	dst->pts = src->pts;
	dst->pkt_dts = src->pkt_dts;
	dst->pkt_pts = src->pkt_pts;
}

int main(int argc, char **argv)
{
	//ffmepg
	int y_size, i, videoindex;
	AVFormatContext *pFormatCtx = NULL;
	AVCodecContext *pCodecCtx = NULL;
	AVCodec *pCodec = NULL;
	AVFrame *pFrame = NULL;
	AVFrame *pFrameYUV = NULL;
	AVPacket *packet = NULL;
	struct SwsContext *img_convert_ctx = NULL;

	//SDL
	int screen_w = 0, screen_h = 0;
	SDL_Window *screen;
	SDL_Renderer* sdlRenderer;
	SDL_Texture* sdlTexture;
	SDL_Rect sdlRect;

	//File for decoder
	FILE *fp_yuv = NULL;
	int ret, got_picture;

	char filepath[1024] = {0};
	printf("\nPlease input file path:\n");
	scanf("%s", filepath);

	av_register_all();

	pFormatCtx = avformat_alloc_context();
	if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0){
		printf("Couldn't open input stream.\n");
		goto ERROR;
	}
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0){
		printf("Couldn't find stream information.\n");
		goto ERROR;
	}
	videoindex = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++)
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO){
			videoindex = i;
			break;
		}
	if (videoindex == -1){
		printf("Didn't find a video stream.\n");
		goto ERROR;
	}
	pCodecCtx = pFormatCtx->streams[videoindex]->codec;

#if HARDWARE_DECODER_CUVID
	AVHWAccel *hwaccel = NULL;
	while ((hwaccel = av_hwaccel_next(hwaccel)) != NULL)
	{
		if (strcmp("h264_qsv", hwaccel->name) == 0)
		{
			pCodec = avcodec_find_decoder_by_name(hwaccel->name);
			//pCodecCtx->get_format = get_hwaccel_format;
			//pCodecCtx->opaque = hwaccel;
			break;
		}
	}
#else
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (pCodec == NULL){
		printf("Codec not found.\n");
		goto ERROR;
	}
#if HARDWARE_DECODER_DXVA2
	InputStream *ist = new InputStream();
	ist->hwaccel_id = HWACCEL_AUTO;
	ist->active_hwaccel_id = HWACCEL_AUTO;
	ist->hwaccel_device = "dxva2";
	ist->dec = pCodec;
	ist->dec_ctx = pCodecCtx;

	pCodecCtx->opaque = ist;
	if (dxva2_init(pCodecCtx) == 0)
	{
		pCodecCtx->get_buffer2 = ist->hwaccel_get_buffer;
		pCodecCtx->get_format = get_hwaccek_format;
		pCodecCtx->thread_safe_callbacks = 1;
		printf("hardware decode by dxva2.");
	}
#endif
#endif

	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0){
		printf("Could not open codec.\n");
		goto ERROR;
	}

	//SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		goto ERROR;
	}

	screen_w = pCodecCtx->width;
	screen_h = pCodecCtx->height;
	y_size = pCodecCtx->width * pCodecCtx->height;
	screen = SDL_CreateWindow("Simplest ffmpeg player Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
									screen_w, screen_h,SDL_WINDOW_OPENGL);
	if (!screen) {
		printf("SDL: could not set video mode - exiting:%s\n", SDL_GetError());
		goto ERROR;
	}

	sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
	sdlTexture  = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, pCodecCtx->width, pCodecCtx->height);
	sdlRect.x = 0;
	sdlRect.y = 0;
	sdlRect.w = screen_w;
	sdlRect.h = screen_h;

	//Output Information
	av_dump_format(pFormatCtx, 0, filepath, 0);

#if OUTPUT_YUV420P 
	static int flag = 1;
	fp_yuv = fopen("output.yuv", "wb+");
#endif  

	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();
	packet = (AVPacket *)av_malloc(sizeof(AVPacket));

	int max_frame_size = avpicture_get_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height);
	uint8_t *max_frame_buffer = (uint8_t *)av_malloc(max_frame_size * sizeof(uint8_t));
	avpicture_fill((AVPicture *)pFrameYUV, max_frame_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height);
	img_convert_ctx = sws_getContext(pCodecCtx->coded_width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);

	while (av_read_frame(pFormatCtx, packet) >= 0){
		if (packet->stream_index == videoindex){
			ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
			if (ret < 0){
				//printf("Decode Error.\n");
				//goto ERROR;
			}
			if (got_picture){
				 
#if HARDWARE_DECODER_DXVA2
				dxva2_get_yuv420_data(pCodecCtx, pFrame, pFrameYUV);
#elif HARDWARE_DECODER_CUVID
				//nv12_to_yuv420(pCodecCtx, pFrame, pFrameYUV);
				sws_scale(img_convert_ctx, (const uint8_t* const*)pFrame->data, pFrame->linesize, 0,
					pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);
#endif

#if OUTPUT_YUV420P
				if (flag == 1){
					flag += 1;
					int y_size = pFrame->linesize[0] * pCodecCtx->height;
#if HARDWARE_DECODER_DXVA2|HARDWARE_DECODER_CUVID
					fwrite(max_frame_buffer, 1, max_frame_size, fp_yuv); 
					//fwrite(pFrameYUV->data[0], 1, y_size, fp_yuv);      //Y 
					//fwrite(pFrameYUV->data[1], 1, y_size / 4, fp_yuv);  //U
					//fwrite(pFrameYUV->data[2], 1, y_size / 4, fp_yuv);  //V
#else
					fwrite(pFrame->data[0], 1, y_size, fp_yuv);      //Y 
					fwrite(pFrame->data[1], 1, y_size / 4, fp_yuv);  //U
					fwrite(pFrame->data[2], 1, y_size / 4, fp_yuv);  //V
#endif
				}
#endif
#if HARDWARE_DECODER_DXVA2|HARDWARE_DECODER_CUVID
				SDL_UpdateYUVTexture(sdlTexture, &sdlRect,
					pFrameYUV->data[0], pFrameYUV->linesize[0],
					pFrameYUV->data[1], pFrameYUV->linesize[1],
					pFrameYUV->data[2], pFrameYUV->linesize[2]);
#else
				SDL_UpdateYUVTexture(sdlTexture, &sdlRect,
					pFrame->data[0], pFrame->linesize[0],
					pFrame->data[1], pFrame->linesize[1],
					pFrame->data[2], pFrame->linesize[2]);
#endif

				SDL_RenderClear(sdlRenderer);
				SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
				SDL_RenderPresent(sdlRenderer);
				//Delay 40ms
				SDL_Delay(40);
			}
		}
		av_free_packet(packet);
	}

ERROR:

#if OUTPUT_YUV420P 
	if (fp_yuv)
		fclose(fp_yuv);
#endif 

	SDL_Quit();

	if (img_convert_ctx)
		sws_freeContext(img_convert_ctx);
	if (pFrame)
		av_frame_free(&pFrame);
	if (pFrameYUV)
		av_frame_free(&pFrameYUV);
	if (pCodecCtx){
		avcodec_close(pCodecCtx);
#if HARDWARE_DECODER_DXVA2
		InputStream *ist = (InputStream *)pCodecCtx->opaque;
		if (ist != NULL){
			ist->hwaccel_uninit(pCodecCtx);
		}
		delete ist;
#endif
	}
	if (pFormatCtx)
		avformat_close_input(&pFormatCtx);
	if (packet)
		av_free_packet(packet);

	system("pause");
	return 0;
}
