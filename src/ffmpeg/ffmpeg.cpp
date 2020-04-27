extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavformat/avio.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>
#include <libavutil/motion_vector.h>
#include <libavutil/frame.h>
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "swscale.lib")
#pragma comment(lib, "avdevice.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "avfilter.lib")
#pragma comment(lib, "postproc.lib")
#pragma comment(lib, "swresample.lib")

#define _CRT_SECURE_NO_WARNINGS
#pragma warning(disable : 4996)

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

int main(int argc, char* argv[]) {
	AVFormatContext* pFormatCtx = NULL;
	AVOutputFormat* pOut = NULL;
	AVFormatContext* pFormatCtxOut = NULL;
	int               i, videoStream, ret;
	AVCodecContext* pCodecCtxOrig = NULL;
	AVCodecContext* pCodecCtx = NULL;
	AVCodecContext* pCodecCtxOut = NULL;
	AVCodec* pCodec = NULL;
	AVCodec* pCodecOut = NULL;
	AVFrame* pFrame = NULL;
	AVFrame* pFrameOut = NULL;
	AVDictionary* pDictionary = NULL;

	AVFrame* pFrameRGB = NULL;

	AVPacket* packet;
	AVPacket* packetOut;
	int               frameFinished;
	int               numBytes;
	uint8_t* buffer = NULL;
	uint8_t* buffer_rgb = NULL;
	struct SwsContext* sws_ctx = NULL;
	struct SwsContext* sws_ctx_RGB = NULL;

	const char* out_filename;
	const char* in_filename;

	//in_filename = "input.mp4";
	//out_filename = "output.mp4";


	if (argc != 3) {
		printf("Please specify the parameters: reverseRGB <path_input_file> <path_output_file>\n");
		printf("Press any key to continue...\n");
		getch();
		return 0;
	}

	in_filename = argv[1];
	out_filename = argv[2];

	// Регистрируем все форматы и кодеки
	av_register_all();

	// Пробуем открыть видео файл
	if (avformat_open_input(&pFormatCtx, in_filename, NULL, NULL) != 0)
		return -1; // Не могу открыть файл

	// Пробуем получить информацию о потоке
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
		return -1;

	// Получаем подробную информацию о файле: продолжительность, битрейд, контейнер и прочее
	av_dump_format(pFormatCtx, 0, in_filename, 0);

	avformat_alloc_output_context2(&pFormatCtxOut, NULL, NULL, out_filename);
	pOut = pFormatCtxOut->oformat;

	for (i = 0; i < pFormatCtx->nb_streams; i++) {
		AVStream* in_stream = pFormatCtx->streams[i];
		AVStream* out_stream = avformat_new_stream(pFormatCtxOut, in_stream->codec->codec);
		if (!out_stream) {
			fprintf(stderr, "Failed allocating output stream\n");
			ret = AVERROR_UNKNOWN;
		}
		ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
		if (ret < 0) {
			fprintf(stderr, "Failed to copy context from input to output stream codec context\n");
		}
		out_stream->codec->codec_tag = 0;
		if (pFormatCtxOut->oformat->flags & AVFMT_GLOBALHEADER)
			out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	av_dump_format(pFormatCtxOut, 0, out_filename, 1);

	if (!(pOut->flags & AVFMT_NOFILE)) {
		ret = avio_open(&pFormatCtxOut->pb, out_filename, AVIO_FLAG_WRITE);
		if (ret < 0) {
			fprintf(stderr, "Could not open output file '%s'", out_filename);
			return -1;
		}
	}
	ret = avformat_write_header(pFormatCtxOut, NULL);
	if (ret < 0) {
		fprintf(stderr, "Error occurred when opening output file\n");
		return -1;
	}

	// Находим первый кадр
	videoStream = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++)
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoStream = i;
			break;
		}
	if (videoStream == -1)
		return -1; // Не нашли

	// Указатель куда будут сохраняться данные 
	pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;

	// Находим подходящий декодер для файла
	pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
	pCodecOut = avcodec_find_encoder(pCodecCtxOrig->codec_id);

	if (pCodec == NULL || pCodecOut == NULL) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1; // Декодер не найден
	}
	// Копируем контекст
	pCodecCtx = avcodec_alloc_context3(pCodec);
	pCodecCtxOut = avcodec_alloc_context3(pCodecOut);

	if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0) {
		fprintf(stderr, "Couldn't copy codec context");
		return -1; // Ошибка копирования
	}

	pCodecCtxOut->time_base = AVRational({ 1, 25 });
	pCodecCtxOut->framerate = AVRational({ 25, 1 });
	pCodecCtxOut->pix_fmt = AV_PIX_FMT_YUV420P;
	pCodecCtxOut->width = pCodecCtx->width;
	pCodecCtxOut->height = pCodecCtx->height;
	pCodecCtxOut->bit_rate = pCodecCtx->bit_rate;


	// Открываем кодеки
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
		return -1; // Не смогли открыть кодек
	if (avcodec_open2(pCodecCtxOut, pCodecOut, NULL) < 0)
		return -1; // Не смогли открыть кодек

	// Здесь будет храниться входящий кадр
	pFrame = av_frame_alloc();

	// Здесь будет храниться исходящий кадр
	pFrameOut = av_frame_alloc();
	pFrameOut->format = pCodecCtxOut->pix_fmt;
	pFrameOut->width = pCodecCtxOut->width;
	pFrameOut->height = pCodecCtxOut->height;

	// Здесь храниться кадр преобразованный в RGB
	pFrameRGB = av_frame_alloc();
	pFrameRGB->format = pCodecCtxOut->pix_fmt;
	pFrameRGB->width = pCodecCtxOut->width;
	pFrameRGB->height = pCodecCtxOut->height;

	// Здесь храниться исходящий пакет
	packetOut = av_packet_alloc();
	packet = av_packet_alloc();

	if (pFrameRGB == NULL)
		return -1;

	// Определяем необходимый размер буфера для кадра в RGB и выделяем память
	numBytes = avpicture_get_size(AV_PIX_FMT_RGB24, pCodecCtxOut->width,
		pCodecCtxOut->height);
	buffer_rgb = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
	//  Связываем кадр с вновь выделенным буфером
	avpicture_fill((AVPicture*)pFrameRGB, buffer_rgb, AV_PIX_FMT_RGB24,
		pCodecCtxOut->width, pCodecCtxOut->height);

	// Инициализируем SWS context для программного преобразования полученного кадра в RGB
	sws_ctx_RGB = sws_getContext(pCodecCtxOut->width,
		pCodecCtxOut->height,
		pCodecCtxOut->pix_fmt,
		pCodecCtxOut->width,
		pCodecCtxOut->height,
		AV_PIX_FMT_RGB24,
		SWS_BICUBIC,
		NULL,
		NULL,
		NULL
	);

	// Определяем необходимый размер буфера для исходящего кадра и выделяем память
	numBytes = avpicture_get_size(AV_PIX_FMT_YUV420P, pCodecCtxOut->width,
		pCodecCtxOut->height);
	buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
	//  Связываем кадр с вновь выделенным буфером
	avpicture_fill((AVPicture*)pFrameOut, buffer, AV_PIX_FMT_YUV420P,
		pCodecCtxOut->width, pCodecCtxOut->height);

	// Инициализируем SWS context для программного преобразования кадра RGB в YUV420
	sws_ctx = sws_getContext(pCodecCtxOut->width,
		pCodecCtxOut->height,
		AV_PIX_FMT_RGB24,
		pCodecCtxOut->width,
		pCodecCtxOut->height,
		AV_PIX_FMT_YUV420P,
		NULL,
		NULL,
		NULL,
		NULL
	);

	i = 0;
	printf("Please wait...\n");
	
	int counte_frame = int(pFormatCtx->duration / 1000000)*25;
	int c(1);

	while (av_read_frame(pFormatCtx, packet) >= 0) {
		AVStream* in_stream, * out_stream;
		in_stream = pFormatCtx->streams[packet->stream_index];
		out_stream = pFormatCtxOut->streams[packet->stream_index];
		// Это пакет видео потока?
		if (packet->stream_index == videoStream) {
			// Декодируем видео кадр
			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, packet);
			// Мы получили видео кадр?
			if (frameFinished) {
				// Преобразуем кадр в RGB
				sws_scale(sws_ctx_RGB, (uint8_t const * const *)pFrame->data,
					pFrame->linesize, 0, pCodecCtxOut->height,
					pFrameRGB->data, pFrameRGB->linesize);
				av_free_packet(packetOut);
				//r = av_frame_make_writable(pFrameRGB);//проверка

				//инвертируем параметры цвета (имитация обработки)
				for (int w = 0; w < 3 * pCodecCtxOut->height*pCodecCtxOut->width; w++) {
					pFrameRGB->data[0][w] ^= 0xff;
				}

				// Преобразуем кадр в YUV420
				int inLinesize[1] = { 3 * pCodecCtxOut->width}; // RGB stride
				sws_scale(sws_ctx, (uint8_t const * const *)pFrameRGB->data,
					inLinesize, 0, pCodecCtxOut->height,
					pFrameOut->data, pFrameOut->linesize);
				pFrameOut->pts = i;
				i += 1;
				//кодируем кадр
				avcodec_encode_video2(pCodecCtxOut, packetOut, pFrameOut, &frameFinished);
				//сохраняем пакет
				packetOut->pts = av_rescale_q_rnd(packet->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_PASS_MINMAX);
				packetOut->dts = av_rescale_q_rnd(packet->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_PASS_MINMAX);
				packetOut->duration = av_rescale_q(packet->duration, in_stream->time_base, out_stream->time_base);
				packetOut->pos = -1;
				av_write_frame(pFormatCtxOut, packetOut);
				
				printf("Complite: %f%s", (double(i) / double(counte_frame))*100, "%");
				printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
				//if (i % 25 == 0) printf("Second %d\n",i/25);
			}
			else {
				/*packet->pts = av_rescale_q_rnd(packet->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_PASS_MINMAX);
				packet->dts = av_rescale_q_rnd(packet->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_PASS_MINMAX);
				packet->duration = av_rescale_q(packet->duration, in_stream->time_base, out_stream->time_base);
				packet->pos = -1;
				av_write_frame(pFormatCtxOut, packet);*/
			}
		}
		else {
			packet->pts = av_rescale_q_rnd(packet->pts, in_stream->time_base, out_stream->time_base, AV_ROUND_PASS_MINMAX);
			packet->dts = av_rescale_q_rnd(packet->dts, in_stream->time_base, out_stream->time_base, AV_ROUND_PASS_MINMAX);
			packet->duration = av_rescale_q(packet->duration, in_stream->time_base, out_stream->time_base);
			packet->pos = -1;
			av_write_frame(pFormatCtxOut, packet);
		}
		av_packet_unref(packet);
	}
	printf("Complite: 100%s        \n","%");

	av_write_trailer(pFormatCtxOut);
	avformat_close_input(&pFormatCtx);
	/* close output */
	if (pFormatCtxOut && !(pOut->flags & AVFMT_NOFILE))
		avio_closep(&pFormatCtxOut->pb);
	avformat_free_context(pFormatCtxOut);
	if (ret < 0 && ret != AVERROR_EOF) {
		return 1;
	}


	// Освобождение памяти и закрытие кодеков
	av_free(buffer);
	av_frame_free(&pFrameRGB);
	av_frame_free(&pFrameOut);
	av_frame_free(&pFrame);

	avcodec_close(pCodecCtx);
	avcodec_close(pCodecCtxOut);
	avcodec_close(pCodecCtxOrig);

	avformat_close_input(&pFormatCtx);
	printf("Press any key to continue...\n");
	getch();
	return 0;
}