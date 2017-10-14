#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include <d3d9.h>
#include <dxva2api.h>

static int get_buffer(struct AVCodecContext *avctx, AVFrame *pic)
{
	int ret;
	DXVA2Context *ctx = (DXVA2Context *)avctx->priv_data;
	dxva2_context *dxva2_ctx = &ctx->dxva2_ctx;
	avctx->pix_fmt = ctx->pix_fmt;
	ff_init_buffer_info(avctx, pic);
	if ((ret = ctx->get_buffer(avctx, pic)) < 0) {
		av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
		return ret;
	}
	if (dxva2_ctx) {
		if (av_get_dxva2_surface(dxva2_ctx, pic)) {
			av_log(NULL, AV_LOG_ERROR, "VaGrabSurface failed");
			return -1;
		}
		return 0;
	}
	else {
		av_log(NULL, AV_LOG_ERROR, "No dxva2 context, get buffer failed");
		return -1;
	}
}

static void release_buffer(struct AVCodecContext *avctx, AVFrame *pic)
{
	DXVA2_DecoderContext *ctx = (DXVA2_DecoderContext *)avctx->priv_data;
	dxva2_context *dxva2_ctx = &ctx->dxva2_ctx;
	if (dxva2_ctx) {
		av_release_dxva2_surface(dxva2_ctx, pic);
	}
	ctx->release_buffer(avctx, pic);
	for (int i = 0; i < 4; i++)
		pic->data[i] = NULL;
}

static enum PixelFormat get_format(AVCodecContext *p_context,
	const enum PixelFormat *pi_fmt)
{
	return AV_PIX_FMT_DXVA2_VLD;
}
static int check_format(AVCodecContext *avctx)
{
	uint8_t *pout;
	int psize;
	int index;
	H264Context *h;
	int ret = -1;
	AVCodecParserContext *parser = NULL;
	/* check if support */
	switch (avctx->codec_id) {
	case AV_CODEC_ID_H264:
		/* init parser & parse file */
		parser = av_parser_init(avctx->codec->id);
		if (!parser) {
			av_log(avctx, AV_LOG_ERROR, "Failed to open parser.\n");
			break;
		}
		parser->flags = PARSER_FLAG_COMPLETE_FRAMES;
		index = av_parser_parse2(parser, avctx, &pout, &psize, NULL, 0, 0, 0, 0);
		if (index < 0) {
			av_log(avctx, AV_LOG_ERROR, "Failed to parse this file.\n");
			av_parser_close(parser);
		}
		h = parser->priv_data;
		if (8 == h->sps.bit_depth_luma) {
			if (!CHROMA444 && !CHROMA422) {
				// only this will decoder switch to hwaccel
				av_parser_close(parser);
				ret = 0;
				break;
			}
		}
		else {
			av_log(avctx, AV_LOG_ERROR, "Unsupported file.\n");
			av_parser_close(parser);
			break;
		}
		break;
	case AV_CODEC_ID_MPEG2VIDEO:
		if (CHROMA_420 == get_mpeg2_video_format(avctx)) {
			ret = 0;
			break;
		}
		else {
			av_log(avctx, AV_LOG_ERROR, "Unsupported file.\n");
			break;
		}
	default:
		ret = 0;
		break;
	}
	return ret;
}

int ff_dxva2dec_decode(AVCodecContext *avctx, void *data, int *got_frame,
	AVPacket *avpkt, AVCodec *codec)
{
	DXVA2_DecoderContext *ctx = (DXVA2_DecoderContext *)avctx->priv_data;
	AVFrame *pic = data;
	int ret;
	ret = codec->decode(avctx, data, got_frame, avpkt);
	if (*got_frame) {
		pic->format = ctx->pix_fmt;
		av_extract_dxva2(&(ctx->dxva2_ctx), pic);
	}
	avctx->pix_fmt = ctx->pix_fmt;
	return ret;
}

int ff_dxva2dec_close(AVCodecContext *avctx, AVCodec *codec)
{
	DXVA2_DecoderContext *ctx = avctx->priv_data;
	/* release buffers and decoder */
	av_release_dxva2(&ctx->dxva2_ctx);
	/* close decoder */
	codec->close(avctx);
	return 0;
}


int ff_dxva2dec_init(AVCodecContext *avctx, AVCodec *hwcodec, AVCodec *codec)
{
	DXVA2_DecoderContext *ctx = (DXVA2_DecoderContext *)avctx->priv_data;
	dxva2_context *dxva2_ctx = (dxva2_context *)(&ctx->dxva2_ctx);
	int ret;
	ctx->initialized = 0;
	/* init pix_fmts of codec */
	if (!(hwcodec->pix_fmts)) {
		hwcodec->pix_fmts = dxva2_pixfmts;
	}
	/* check if DXVA2 supports this file */
	if (check_format(avctx) < 0)
		goto failed;

	/* init vda */
	memset(dxva2_ctx, 0, sizeof(dxva2_context));
	ret = av_create_dxva2(avctx->codec_id, dxva2_ctx);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "create dxva2 error\n");
		return 0;
	}
	ctx->pix_fmt = avctx->get_format(avctx, avctx->codec->pix_fmts);
	ret = av_setup_dxva2(dxva2_ctx, &avctx->hwaccel_context, &avctx->pix_fmt, avctx->width, avctx->height);
	if (ret < 0) {
		av_log(NULL, AV_LOG_ERROR, "error DXVA setup %d\n", ret);
		goto failed;
	}
	/* changes callback functions */
	ctx->get_buffer = avctx->get_buffer;
	avctx->get_format = get_format;
	avctx->get_buffer = get_buffer;
	avctx->release_buffer = release_buffer;
	/* init decoder */
	ret = codec->init(avctx);
	if (ret < 0) {
		av_log(avctx, AV_LOG_ERROR, "Failed to open decoder.\n");
		goto failed;
	}
	ctx->initialized = 1;
	return 0;
failed:
	ff_dxva2dec_close(avctx, codec);
	return -1;
}

void ff_dxva2dec_flush(AVCodecContext *avctx, AVCodec *codec)
{
	return codec->flush(avctx);
}


//////////////////////////////////////////////////////////////////////////////////////

static int h264_dxva2dec_decode(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
{
	return ff_dxva2dec_decode(avctx, data, got_frame, avpkt, &ff_h264_decoder);
}

static av_cold int h264_dxva2dec_close(AVCodecContext *avctx)
{
	return ff_dxva2dec_close(avctx, &ff_h264_decoder);
}

static av_cold int h264_dxva2dec_init(AVCodecContext *avctx)
{
	return ff_dxva2dec_init(avctx, &ff_h264_dxva2_decoder, &ff_h264_decoder);
}

static void h264_dxva2dec_flush(AVCodecContext *avctx)
{
	ff_dxva2dec_flush(avctx, &ff_h264_decoder);
}

AVCodec ff_h264_dxva2_decoder = {
	.name = "h264_dxva2",
	.type = AVMEDIA_TYPE_VIDEO,
	.id = AV_CODEC_ID_H264,
	.priv_data_size = sizeof(DXVA2_DecoderContext),
	.init = h264_dxva2dec_init,
	.close = h264_dxva2dec_close,
	.decode = h264_dxva2dec_decode,
	.capabilities = CODEC_CAP_DELAY,
	.flush = h264_dxva2dec_flush,
	.long_name = NULL_IF_CONFIG_SMALL("H.264 (DXVA2 acceleration)"),
};