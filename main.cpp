#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <ctime>

#ifdef _MSC_VER
#include "inttypes.h"
#endif
#include <stdint.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/motion_vector.h>
}

#include <string>
#include <algorithm>
#include <vector>
#include <stdexcept>

using namespace std;

void ffmpeg_print_error(int err) // copied from cmdutils.c, originally called print_error
{
	char errbuf[128];
	const char *errbuf_ptr = errbuf;

	if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
		errbuf_ptr = strerror(AVUNERROR(err));
	av_log(NULL, AV_LOG_ERROR, "ffmpeg_print_error: %s\n", errbuf_ptr);
}

void ffmpeg_init(const char* videoPath, AVFrame*& pFrame, AVFormatContext*& pFormatCtx, AVStream*& pVideoStream, int& videoStreamIndex, size_t& frameWidth, size_t& frameHeight)
{
	av_register_all();

	pFrame = av_frame_alloc();
	pFormatCtx = avformat_alloc_context();
	videoStreamIndex = -1;

	int err = 0;

	if ((err = avformat_open_input(&pFormatCtx, videoPath, NULL, NULL)) != 0)
	{
		ffmpeg_print_error(err);
		throw std::runtime_error("Couldn't open file. Possibly it doesn't exist.");
	}

	if ((err = avformat_find_stream_info(pFormatCtx, NULL)) < 0)
	{
		ffmpeg_print_error(err);
		throw std::runtime_error("Stream information not found.");
	}

	for(int i = 0; i < pFormatCtx->nb_streams; i++)
	{
		AVCodecContext *enc = pFormatCtx->streams[i]->codec;
		if( AVMEDIA_TYPE_VIDEO == enc->codec_type && videoStreamIndex < 0 )
		{
			AVCodec *pCodec = avcodec_find_decoder(enc->codec_id);
			AVDictionary *opts = NULL;
			av_dict_set(&opts, "flags2", "+export_mvs", 0);
			if (!pCodec || avcodec_open2(enc, pCodec, &opts) < 0)
				throw std::runtime_error("Codec not found or cannot open codec.");

			videoStreamIndex = i;
			pVideoStream = pFormatCtx->streams[i];
			frameWidth = enc->width;
			frameHeight = enc->height;

			break;
		}
	}

	if(videoStreamIndex == -1)
		throw std::runtime_error("Video stream not found.");
}

bool process_frame(AVPacket *pkt, AVFrame* pFrame, AVStream* pVideoStream)
{
	av_frame_unref(pFrame);

	int got_frame = 0;
	int ret = avcodec_decode_video2(pVideoStream->codec, pFrame, &got_frame, pkt);
	if (ret < 0)
		return false;

	ret = FFMIN(ret, pkt->size); /* guard against bogus return values */
	pkt->data += ret;
	pkt->size -= ret;
	return got_frame > 0;
}

bool read_packets(AVFrame* pFrame, AVFormatContext* pFormatCtx, AVStream* pVideoStream, int videoStreamIndex)
{
	static bool initialized = false;
	static AVPacket pkt, pktCopy;

	while(true)
	{
		if(initialized)
		{
			if(process_frame(&pktCopy, pFrame, pVideoStream))
				return true;
			else
			{
				av_free_packet(&pkt);
				initialized = false;
			}
		}

		int ret = av_read_frame(pFormatCtx, &pkt);
		if(ret != 0)
			break;

		initialized = true;
		pktCopy = pkt;
		if(pkt.stream_index != videoStreamIndex)
		{
			av_free_packet(&pkt);
			initialized = false;
			continue;
		}
	}

	return process_frame(&pkt, pFrame, pVideoStream);
}

bool read_frame(AVFrame* pFrame, AVFormatContext* pFormatCtx, AVStream* pVideoStream, int videoStreamIndex, int64_t& pts, char& pictType, vector<AVMotionVector>& motion_vectors)
{
	if(!read_packets(pFrame, pFormatCtx, pVideoStream, videoStreamIndex))
		return false;
	
	pictType = av_get_picture_type_char(pFrame->pict_type);
	// fragile, consult fresh f_select.c and ffprobe.c when updating ffmpeg
	pts = pFrame->pkt_pts != AV_NOPTS_VALUE ? pFrame->pkt_pts : (pFrame->pkt_dts != AV_NOPTS_VALUE ? pFrame->pkt_dts : pts + 1);
	bool noMotionVectors = av_frame_get_side_data(pFrame, AV_FRAME_DATA_MOTION_VECTORS) == NULL;
	if(!noMotionVectors)
	{
		// reading motion vectors, see ff_print_debug_info2 in ffmpeg's libavcodec/mpegvideo.c for reference and a fresh doc/examples/extract_mvs.c
		AVFrameSideData* sd = av_frame_get_side_data(pFrame, AV_FRAME_DATA_MOTION_VECTORS);
		AVMotionVector* mvs = (AVMotionVector*)sd->data;
		int mvcount = sd->size / sizeof(AVMotionVector);
		motion_vectors = vector<AVMotionVector>(mvs, mvs + mvcount);
	}
	else
	{
		motion_vectors = vector<AVMotionVector>();
	}

	return true;
}

struct FrameInfo
{
	const static int GRID_STEP = 16;
	const static int MAX_GRID_SIZE = 512;

	int dx[MAX_GRID_SIZE][MAX_GRID_SIZE];
	int dy[MAX_GRID_SIZE][MAX_GRID_SIZE];
	bool occupancy[MAX_GRID_SIZE][MAX_GRID_SIZE];
	int64_t Pts;
	int FrameIndex;
	char PictType;
	pair<size_t, size_t> Shape;
	bool Empty;

	FrameInfo()
	{
		memset(dx, 0, sizeof(dx));
		memset(dy, 0, sizeof(dy));
		memset(occupancy, 0, sizeof(occupancy));
		Empty = true;
	}

	void InterpolateFlow(FrameInfo& prev, FrameInfo& next)
	{
		Empty = false;
		for(int i = 0; i < Shape.first; i++)
		{
			for(int j = 0; j < Shape.second; j++)
			{
				dx[i][j] = (prev.dx[i][j] + next.dx[i][j]) / 2;
				dy[i][j] = (prev.dy[i][j] + next.dy[i][j]) / 2;
			}
		}
	}

	void Print()
	{
		printf("# pts=%ld frame_index=%d pict_type=%c output_type=arranged shape=%lux%lu\n", Pts, FrameIndex, PictType, Shape.first, Shape.second);
		for(int i = 0; i < Shape.first; i++)
		{
			for(int j = 0; j < Shape.second; j++)
			{
				printf("%d\t", dx[i][j]);
			}
			printf("\n");
		}
		for(int i = 0; i < Shape.first; i++)
		{
			for(int j = 0; j < Shape.second; j++)
			{
				printf("%d\t", dy[i][j]);
			}
			printf("\n");
		}
	}
};

void output_vectors_raw(int frameIndex, int64_t pts, char pictType, vector<AVMotionVector>& motionVectors)
{
	printf("# pts=%ld frame_index=%d pict_type=%c output_type=raw shape=%lux4\n", pts, frameIndex, pictType, motionVectors.size());
	for(int i = 0; i < motionVectors.size(); i++)
	{
		AVMotionVector& mv = motionVectors[i];
		int mvdx = mv.src_x - mv.dst_x;
		int mvdy = mv.src_y - mv.dst_y;

		printf("%d\t%d\t%d\t%d\n", mv.src_x, mv.src_y, mvdx, mvdy);
	}
}

void output_vectors_std(int frameIndex, int64_t pts, char pictType, vector<AVMotionVector>& motionVectors, int frameWidth, int frameHeight)
{
	static vector<FrameInfo> prev;

	FrameInfo cur;
	cur.FrameIndex = frameIndex;
	cur.Pts = pts;
	cur.PictType = pictType;
	cur.Shape = make_pair(min(frameHeight / FrameInfo::GRID_STEP, FrameInfo::MAX_GRID_SIZE), min(frameWidth / FrameInfo::GRID_STEP, FrameInfo::MAX_GRID_SIZE));

	for(int i = 0; i < motionVectors.size(); i++)
	{
		AVMotionVector& mv = motionVectors[i];
		int mvdx = mv.src_x - mv.dst_x;
		int mvdy = mv.src_y - mv.dst_y;

		size_t i_clipped = mv.src_y / FrameInfo::GRID_STEP;
		size_t j_clipped = mv.src_x / FrameInfo::GRID_STEP;
		i_clipped = max(size_t(0), min(i_clipped, cur.Shape.first - 1)); 
		j_clipped = max(size_t(0), min(j_clipped, cur.Shape.second - 1));

		cur.Empty = false;
		cur.dx[i_clipped][j_clipped] = mvdx;
		cur.dy[i_clipped][j_clipped] = mvdy;
		cur.occupancy[i_clipped][j_clipped] = false;
	}

	if(!motionVectors.empty())
	{
		if(prev.size() == 2 && prev.front().Empty == false)
		{
			prev.back().InterpolateFlow(prev.front(), cur);
			prev.back().Print();
		}
		else
		{
			for(int i = 0; i < prev.size(); i++)
				prev[i].Print();
		}
		prev.clear();
		cur.Print();
	}

	if(frameIndex == -1)
	{
		for(int i = 0; i < prev.size(); i++)
			prev[i].Print();
	}

	prev.push_back(cur);
}


int main(int argc, const char* argv[])
{
	bool ARG_OUTPUT_RAW_MOTION_VECTORS = false;
	const char* ARG_VIDEO_PATH = NULL;
	for(int i = 1; i < argc; i++)
	{
		if(strcmp(argv[i], "--raw") == 0)
			ARG_OUTPUT_RAW_MOTION_VECTORS = true;
		else
			ARG_VIDEO_PATH = argv[i];
	}
	if(ARG_VIDEO_PATH == NULL)
	{
		fprintf(stderr, "Usage: mpegflow [--raw] videoPath\n\n  Specify --raw flag to prevent motion vectors from being arranged in a matrix.\n\n");
		exit(1);
	}

	AVFrame* pFrame;
	AVFormatContext* pFormatCtx;
	AVStream* pVideoStream;
	int videoStreamIndex;
	size_t frameWidth, frameHeight;

	ffmpeg_init(ARG_VIDEO_PATH, pFrame, pFormatCtx, pVideoStream, videoStreamIndex, frameWidth, frameHeight);
		
	int64_t pts;
	char pictType;
	vector<AVMotionVector> motionVectors;

	for(int frameIndex = 1; read_frame(pFrame, pFormatCtx, pVideoStream, videoStreamIndex, pts, pictType, motionVectors); frameIndex++)
	{
		if(ARG_OUTPUT_RAW_MOTION_VECTORS)
			output_vectors_raw(frameIndex, pts, pictType, motionVectors);
		else
			output_vectors_std(frameIndex, pts, pictType, motionVectors, frameWidth, frameHeight);
	}
	if(ARG_OUTPUT_RAW_MOTION_VECTORS == false)
		output_vectors_std(-1, pts, pictType, motionVectors, frameWidth, frameHeight);
}