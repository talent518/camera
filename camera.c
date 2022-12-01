#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>

#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include <SDL.h>
#include <pthread.h>
#include <unistd.h>

#define VIDEO_DEV "/dev/video0"
char file_name[40];//视频编码文件名

#define STREAM_FRAME_RATE 25 /*每秒25帧*/
#define STREAM_PIX_FMT AV_PIX_FMT_YUV420P /*图像格式yuv420p*/
#define STREAM_DURATION 10.0 /*录制时间10s*/
pthread_mutex_t fastmutex = PTHREAD_MUTEX_INITIALIZER;//互斥锁
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;//条件变量
int width;
int height;
int size;
int mp4_decode_stat=0;
unsigned char *rgb_buff=NULL;
unsigned char video_flag=1;
void *Video_CollectImage(void *arg);
void *Video_savemp4(void*arg);
typedef enum
{
	false=0,
	true,
}bool;
typedef struct OutputStream
{
	AVStream *st;
	AVCodecContext *enc;
	int64_t next_pts;/*将生成的下一帧的pts*/
	AVFrame *frame;/*保存编解码数据*/
	AVFrame *tmp_frame;
	struct SwsContext *sws_ctx;
	struct SwrContext *swr_ctx;
}OutputStream;
typedef struct IntputDev
{
	AVCodecContext *pcodecCtx;
	AVCodec *pCodec;
	AVFormatContext *v_ifmtCtx;
	int videoindex;//视频帧ID
	struct SwsContext *img_convert_ctx;
	AVPacket *in_packet;
	AVFrame *pFrame,*pFrameYUV;
}IntputDev;
IntputDev video_input={0};//视频输入流
//添加水印
int waterMark(AVFrame *frame_in,AVFrame *frame_out,int w,int h,const char *str)
{
	int ret;
	/*根据名字获取ffmegding定义的filter*/
	const AVFilter *buffersrc=avfilter_get_by_name("buffer");//原始数据
	const AVFilter *buffersink=avfilter_get_by_name("buffersink");//处理后的数据
	/*动态分配AVFilterInOut空间*/
	AVFilterInOut *outputs=avfilter_inout_alloc();
	AVFilterInOut *inputs=avfilter_inout_alloc();	
	/*创建AVFilterGraph,分配空间*/
	AVFilterGraph *filter_graph;//对filters系统的整体管理结构体
	filter_graph = avfilter_graph_alloc();
	enum AVPixelFormat pix_fmts[]={AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};//设置格式
	/*过滤器参数：解码器的解码帧将被插入这里。*/
	char args[256];
	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		w,h,AV_PIX_FMT_YUV420P,1,25,1,1);//图像宽高，格式，帧率，画面横纵比
	/*创建过滤器上下文,源数据AVFilterContext*/
	AVFilterContext *buffersrc_ctx;
	ret=avfilter_graph_create_filter(&buffersrc_ctx,buffersrc,"in",args,NULL,filter_graph);
	if(ret<0)
	{
		printf("创建src过滤器上下文失败AVFilterContext\n");
		return -1;
	}		
	/*创建过滤器上下文，处理后数据buffersink_params*/
	AVBufferSinkParams *buffersink_params;
	buffersink_params=av_buffersink_params_alloc();
	buffersink_params->pixel_fmts=pix_fmts;//设置格式
	AVFilterContext *buffersink_ctx;
	ret=avfilter_graph_create_filter(&buffersink_ctx,buffersink,"out",NULL,buffersink_params,filter_graph);
	av_free(buffersink_params);
	if(ret<0)
	{
		printf("创建sink过滤器上下文失败AVFilterContext\n");
		return -2;
	}	
	/*过滤器链输入/输出链接列表*/
	outputs->name       =av_strdup("in");
	outputs->filter_ctx =buffersrc_ctx;
	outputs->pad_idx    =0;
	outputs->next		=NULL;

	inputs->name		=av_strdup("out");
	inputs->filter_ctx	=buffersink_ctx;
	inputs->pad_idx    =0;
	inputs->next		=NULL;
	char filter_desrc[200]={0};//要添加的水印数据
	snprintf(filter_desrc,sizeof(filter_desrc),"drawtext=fontfile=msyhbd.ttc:fontcolor=red:fontsize=40:x=50:y=20:text='%s\nIT_阿水'",str);
	if(avfilter_graph_parse_ptr(filter_graph,filter_desrc,&inputs,&outputs, NULL)<0)//设置过滤器数据内容
	{
		printf("添加字符串信息失败\n");
		return -3;
	}
	/*检测配置信息是否正常*/
	if(avfilter_graph_config(filter_graph,NULL)<0)
	{
		printf("配置信息有误\n");
		return -4;
	}	
	#if 0
	/*
		查找要在使用的过滤器，将要触处理的数据添加到过滤器
		注意：时间若从外面传入(即144行数据已完整)，则此处不需要查找，直接添加即可，否则需要添加下面代码
	*/
	AVFilterContext* filter_ctx;//上下文
	int parsed_drawtext_0_index = -1;
	 for(int i=0;i<filter_graph->nb_filters;i++)//查找使用的过滤器
	 {
		 AVFilterContext *filter_ctxn=filter_graph->filters[i];
		 printf("[%s %d]:filter_ctxn_name=%s\n",__FUNCTION__,__LINE__,filter_ctxn->name);
		 if(!strcmp(filter_ctxn->name,"Parsed_drawtext_0"))
		 {
			parsed_drawtext_0_index=i;
		 }
	 }
	 if(parsed_drawtext_0_index==-1)
	 {
		printf("[%s %d]:no Parsed_drawtext_0\n",__FUNCTION__,__LINE__);//没有找到过滤器
	 }
	 filter_ctx=filter_graph->filters[parsed_drawtext_0_index];//保存找到的过滤器
	 
		/*获取系统时间，将时间加入到过滤器*/
		char sys_time[64];
		time_t sec,sec2;	 
		sec=time(NULL);
		if(sec!=sec2)
		{
			sec2=sec;
			struct tm* today = localtime(&sec2);	
			strftime(sys_time, sizeof(sys_time), "%Y/%m/%d %H\\:%M\\:%S", today);       //24小时制
		}
		av_opt_set(filter_ctx->priv, "text", sys_time, 0 );  //设置text到过滤器
	 #endif

	/*往源滤波器buffer中输入待处理数据*/
	 if(av_buffersrc_add_frame(buffersrc_ctx,frame_in)<0)
	 {
		return -5;
	 }
	 /*从滤波器中输出处理数据*/
	 if(av_buffersink_get_frame(buffersink_ctx, frame_out)<0)
	 {
		return -6;
	 }
	avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    avfilter_graph_free(&filter_graph);
	return 0;
}

/*添加一个输出流*/
static void add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id)
{
	AVCodecContext *c;
	int i;
	/*查找编码器*/
	*codec=avcodec_find_encoder(codec_id);
	if(*codec==NULL)
	{
		printf("Could not find encoder for ' %s' \n",avcodec_get_name(codec_id));
		exit(1);
	}
	/*向媒体文件添加新流。*/
	ost->st=avformat_new_stream(oc,NULL);
	if(ost->st==NULL)
	{
		printf("Could not allocate stream \n");
		exit(1);
	}
	ost->st->id=oc->nb_streams-1;
	/*分配AvcodeContext并将其字段设置为默认值*/
	c=avcodec_alloc_context3(*codec);
	if(c==NULL)
	{
		printf("avcodec_alloc_context3 failed \n");
	}
	ost->enc=c;		
	switch((*codec)->type)
	{
		case AVMEDIA_TYPE_AUDIO:
			
			break;	
		case AVMEDIA_TYPE_VIDEO:/*视频流*/
		
			c->codec_id=codec_id;
			c->bit_rate=2500000;//比特率
			/*分辨率必须是2的倍数。*/
			c->width=width;
			c->height=height;
			/*时基：这是时间的基本单位（秒）
				其中帧时间戳被表示。对于固定fps内容，时基应为1/帧率，时间戳增量应与1相同*/
			ost->st->time_base=(AVRational){1,STREAM_FRAME_RATE};
			c->time_base=ost->st->time_base;
			c->gop_size=12;/*最多每12帧发射一帧*/
			c->pix_fmt=STREAM_PIX_FMT;/*图像格式*/
			if(c->codec_id == AV_CODEC_ID_MPEG2VIDEO)
			{
				/*
				为了测试，我们还添加了B帧
				*/
				c->max_b_frames=2;
			}
			if(c->codec_id == AV_CODEC_ID_MPEG1VIDEO)
			{
				/*
				需要避免使用某些系数溢出的宏块。
				这种情况不会发生在普通视频中，只会发生在这里
				色度平面的运动与亮度平面不匹配。*/
				c->mb_decision=2;
			}
			break;
		default:
			break;
	}
	/*有些格式希望流头是分开的。*/
	if(oc->oformat->flags & AVFMT_GLOBALHEADER)
	{
		c->flags|=AV_CODEC_FLAG_GLOBAL_HEADER;
	}
}
/*视频输出*/
static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;
	int ret;
	/*
		分配AVFrame并将其字段设置为默认值。结果
		必须使用av_frame_free（）释放结构。
	*/
	picture=av_frame_alloc();
	if(picture==NULL)
	{
		return  NULL;
	}
	picture->format=pix_fmt;
	picture->width=width;
	picture->height=height;
	/*为帧数据分配缓冲区*/
	ret=av_frame_get_buffer(picture,32);/*缓冲区以32位对齐*/
	if(ret<0)
	{
		printf("Could not allocate frame data\n");/*无法分配帧数据*/
		exit(1);
	}
	return picture;
}

static void open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
	int ret;
	AVCodecContext *c=ost->enc;
	AVDictionary *opt=NULL;
	av_dict_copy(&opt,opt_arg, 0);
	/*初始化AvcodeContext以使用给定的AVCodec*/
	ret=avcodec_open2(c, codec,&opt);
	/*释放为AVDictionary结构分配的所有内存*以及所有键和值。*/
	av_dict_free(&opt);
	if(ret<0)
	{
		printf("could not open video codec :%s\n",av_err2str(ret));//无法打开视频编解码器
		exit(1);
	}
	/*视频输出*/
	ost->frame=alloc_picture(AV_PIX_FMT_YUV420P,c->width,c->height);
	if(ost->frame==NULL)
	{
		printf("could not allocate video frame\n");/*无法分配视频帧*/
		exit(1);
	}
	printf("ost->frame alloc success fmt=%d w=%d h=%d\n",c->pix_fmt,c->width,c->height);
	ost->tmp_frame=NULL;
	if(c->pix_fmt!=AV_PIX_FMT_YUV420P)
	{
		ost->tmp_frame=alloc_picture(AV_PIX_FMT_YUV420P,c->width, c->height);/*视频帧格式*/
		if(ost->tmp_frame==NULL)
		{
			printf("conld not allocate temporary picture\n");/*无法分配临时帧*/
			exit(1);
		}
	}
	/*根据提供的编解码器上下文中的值填充参数结构。*/
	ret=avcodec_parameters_from_context(ost->st->codecpar,c);
	if(ret)
	{
		printf("Could not copy the stream parameters\n");/*无法复制流参数*/
		exit(1);
	}

}
static void log_packet(const AVFormatContext * fmt_ctx, const AVPacket * pkt)
{
	AVRational *time_base=&fmt_ctx->streams[pkt->stream_index]->time_base;
    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           									av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           									av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           									av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           									pkt->stream_index);
}

/*写入数据*/
static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
	/*数据包中的有效时间字段（时间戳/持续时间）从一个时基转换为另一个时基。*/
	av_packet_rescale_ts(pkt,*time_base,st->time_base);
	pkt->stream_index=st->index;
	/*打印信息*/
	log_packet(fmt_ctx, pkt);
	return av_interleaved_write_frame(fmt_ctx, pkt);/*将数据包写入输出媒体文件，确保正确的交织。*/
}
static int write_video_frame(AVFormatContext * oc, OutputStream * ost,AVFrame *frame)
{
	int ret;
	AVCodecContext *c;
	int got_packet=0;
	AVPacket pkt={0};
	if(frame==(void *)-1)return 1;
	c=ost->enc;
	/*使用默认值初始化数据包*/
	av_init_packet(&pkt);
	
	/*对一帧视频进行编码。*/
	ret=avcodec_encode_video2(c,&pkt,frame,&got_packet);
	if(ret<0)
	{
		printf("Error encoding video frame:%s\n",av_err2str(ret));//编码视频流错误
		exit(1);
	}
	printf("--------vidoe pkt.pts=%s\n",av_ts2str(pkt.pts));
	printf("------st.num=%d st.den=%d codec.num=%d codec.den=%d-------\n",ost->st->time_base.num,\
															  ost->st->time_base.den,\
															  c->time_base.num,\
															  c->time_base.den);
	if(got_packet)
	{

		ret=write_frame(oc,&c->time_base,ost->st,&pkt);/*写入流数据*/
	}
	else
	{
		ret=0;
	}
	if(ret<0)
	{
		printf("Error while writing video frame:%s\n",av_err2str(ret));/*写入流出错*/
		exit(1);
	}
	return (frame||got_packet)?0:1;
}
static AVFrame *get_video_frame(OutputStream *ost,IntputDev* input, int *got_pic)
{
	
	int ret,got_picture;
	AVCodecContext *c=ost->enc;
	AVFrame *ret_frame=NULL;
	/*在各自的时基中比较两个时间戳。*/
	if(av_compare_ts(ost->next_pts,c->time_base,STREAM_DURATION, (AVRational){1,1})>=0)
	{
		return  (void*)-1;
	}
	/*确保帧数据可写，尽可能避免数据复制。*/
	if(av_frame_make_writable(ost->frame)<0)
	{
		exit(1);
	}
	/*此函数返回文件中存储的内容，并且不验证是否存在解码器的有效帧。*/
	if(av_read_frame(input->v_ifmtCtx,input->in_packet)>=0)
	{
		if(input->in_packet->stream_index == input->videoindex)
		{
			/*解码一帧视频数据。输入一个压缩编码的结构体AVPacket，输出一个解码后的结构体AVFrame*/
			ret=avcodec_decode_video2(input->pcodecCtx, input->pFrame,&got_picture,input->in_packet);
			*got_pic=got_picture;
			if(ret<0)
			{
				printf("Decode Error.\n");
				av_packet_unref(input->in_packet);
				return NULL;
			}
			if(got_picture)
			{
				sws_scale(input->img_convert_ctx, (const unsigned char * const *)input->pFrame->data,input->pFrame->linesize,0,input->pcodecCtx->height,input->pFrameYUV->data,input->pFrameYUV->linesize);
				sws_scale(input->img_convert_ctx, (const unsigned char * const *)input->pFrame->data,input->pFrame->linesize,0,input->pcodecCtx->height,ost->frame->data,ost->frame->linesize);

				pthread_mutex_lock(&fastmutex);//互斥锁上锁
				memcpy(rgb_buff,input->pFrameYUV->data[0],size);
				pthread_cond_broadcast(&cond);//广播唤醒所有线程
				pthread_mutex_unlock(&fastmutex);//互斥锁解锁
				//ost->frame->pts=ost->next_pts++;

				//水印添加处理
				//frame->frame->format=AV_PIX_FMT_YUV420P;
				AVFrame *frame_out=av_frame_alloc();
				unsigned char *frame_buffer_out;
				frame_buffer_out=(unsigned char *)av_malloc(size);
				av_image_fill_arrays(frame_out->data,frame_out->linesize,frame_buffer_out,AV_PIX_FMT_YUV420P,width,height,32);
				//添加水印,调用libavfilter库实现
				time_t sec;
				sec=time(NULL);
				struct tm* today = localtime(&sec);
				char sys_time[64];
				strftime(sys_time, sizeof(sys_time), "%Y/%m/%d %H\\:%M\\:%S", today);  
				waterMark(ost->frame,frame_out,width,height,sys_time);
				//yuv420p，y表示亮度，uv表示像素颜色
				ost->frame=frame_out;
				ost->frame->pts=ost->next_pts++;

				ret_frame=frame_out;
			}
		}
		av_packet_unref(input->in_packet);
	}
	return ret_frame;
}
static void close_stream(AVFormatContext * oc, OutputStream * ost)
{
	avcodec_free_context(&ost->enc);
	av_frame_free(&ost->frame);
	av_frame_free(&ost->tmp_frame);
	sws_freeContext(ost->sws_ctx);
	swr_free(&ost->swr_ctx);
}

int main()
{
	/*创建摄像头采集线程*/
	pthread_t pthid[2];
    pthread_create(&pthid[0],NULL,Video_CollectImage, NULL);
	pthread_detach(pthid[0]);/*设置分离属性*/
	sleep(1);
	while(1)
	{
		if(width!=0 && height!=0 && size!=0)break;
		if(video_flag==0)return 0;
	}
	printf("image:%d * %d,%d\n",width,height,size);
	unsigned char *rgb_data=malloc(size);
	/*创建mp4视频编码线程*/
	pthread_create(&pthid[1],NULL,Video_savemp4, NULL);
	pthread_detach(pthid[1]);/*设置分离属性*/
 	/*创建窗口 */
	SDL_Window *window=SDL_CreateWindow("SDL_VIDEO", SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,800,480,SDL_WINDOW_ALLOW_HIGHDPI|SDL_WINDOW_RESIZABLE);
    /*创建渲染器*/
	SDL_Renderer *render=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);
	/*清空渲染器*/
	SDL_RenderClear(render);
   /*创建纹理*/
	SDL_Texture*sdltext=SDL_CreateTexture(render,SDL_PIXELFORMAT_IYUV,SDL_TEXTUREACCESS_STREAMING,width,height);
	bool quit=true;
	SDL_Event event;
	SDL_Rect rect;
	int count=0;
	while(quit)
	{
		while(SDL_PollEvent(&event))/*事件监测*/
		{
			if(event.type==SDL_QUIT)/*退出事件*/
			{
				quit=false;
				video_flag=0;
				pthread_cancel(pthid[1]);/*杀死指定线程*/
				pthread_cancel(pthid[0]);/*杀死指定线程*/
				continue;
			}
			else if(event.type == SDL_KEYDOWN)
			{
				 if(event.key.keysym.sym==SDLK_q)//按‘q’保存视频
				 {
					count++;
					snprintf(file_name,sizeof(file_name),"%d.mp4",count);
					mp4_decode_stat=1;
				 }
			}
		}
		if(!video_flag)
		{
			quit=false;
			continue;
		}
		pthread_mutex_lock(&fastmutex);//互斥锁上锁
		pthread_cond_wait(&cond,&fastmutex);
		memcpy(rgb_data,rgb_buff,size);
		pthread_mutex_unlock(&fastmutex);//互斥锁解锁
		SDL_UpdateTexture(sdltext,NULL,rgb_data,width);
		//SDL_RenderCopy(render, sdltext, NULL,NULL); // 拷贝纹理到渲染器
		SDL_RenderCopyEx(render, sdltext,NULL,NULL,0,NULL,SDL_FLIP_NONE);
		SDL_RenderPresent(render); // 渲染
	}
	SDL_DestroyTexture(sdltext);/*销毁纹理*/
    SDL_DestroyRenderer(render);/*销毁渲染器*/
    SDL_DestroyWindow(window);/*销毁窗口 */
    SDL_Quit();/*关闭SDL*/
    pthread_mutex_destroy(&fastmutex);/*销毁互斥锁*/
    pthread_cond_destroy(&cond);/*销毁条件变量*/
	free(rgb_buff);
	free(rgb_data);
	return 0;
}

void *Video_CollectImage(void *arg)
{
	int res=0;
	AVFrame *Input_pFrame=NULL;
	AVFrame *Output_pFrame=NULL;
	printf("pth:%s\n",avcodec_configuration());
	/*注册设备*/
	avdevice_register_all();
	/*查找输入格式*/
	AVInputFormat *ifmt=av_find_input_format("video4linux2");
	if(ifmt==NULL)
	{
		perror("av_find_input_format failed");
		video_flag=0;
		return 0;
	}
	/*打开输入流并读取头部信息*/
	AVFormatContext *ps=NULL;
	//分配一个AVFormatContext。
	ps=avformat_alloc_context();	
	res=avformat_open_input(&ps,VIDEO_DEV,ifmt,NULL);
	if(res)
	{
		perror("open input failed");
		video_flag=0;
		return 0;
	}
	/*查找流信息*/
	res=avformat_find_stream_info(ps,NULL);
	if(res)
	{
		perror("find stream failed");
		video_flag=0;
		return 0;
	}
	/*打印有关输入或输出格式信息*/
	av_dump_format(ps, 0, "video4linux2", 0);
	/*寻找视频流*/
	int videostream=-1;
	videostream=av_find_best_stream(ps,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);
	printf("videostram=%d\n",videostream);
	/*寻找编解码器*/
	AVCodec *video_avcodec=NULL;/*保存解码器信息*/
	AVStream *stream = ps->streams[videostream];
	AVCodecContext *context=stream->codec;
	video_avcodec=avcodec_find_decoder(context->codec_id);
	if(video_avcodec==NULL)
	{
		perror("find video decodec failed");
		video_flag=0;
		return 0;
	}	
	/*初始化音视频解码器*/
	res=avcodec_open2(context,video_avcodec,NULL);
	if(res)
	{
		perror("avcodec_open2 failed");
		video_flag=0;
		return 0;
	}	
	AVPacket *packet=av_malloc(sizeof(AVPacket));/*分配包*/
	AVFrame *frame=av_frame_alloc();/*分配视频帧*/
	AVFrame *frameyuv=av_frame_alloc();/*申请YUV空间*/
	/*分配空间，进行图像转换*/
	width=context->width;
	height=context->height;
	int fmt=context->pix_fmt;/*流格式*/
	size=av_image_get_buffer_size(AV_PIX_FMT_YUV420P,width,height,16);
	unsigned char *buff=NULL;
	printf("w=%d,h=%d,size=%d\n",width,height,size);	
	buff=av_malloc(size);
	rgb_buff=malloc(size);//保存RGB颜色数据
	/*存储一帧图像数据*/
	av_image_fill_arrays(frameyuv->data,frameyuv->linesize,buff,AV_PIX_FMT_YUV420P,width,height, 16);	
	/*转换上下文，使用sws_scale（）执行缩放/转换操作。*/
	struct SwsContext *swsctx=sws_getContext(width,height, fmt,width,height, AV_PIX_FMT_YUV420P,SWS_BICUBIC,NULL,NULL,NULL);
	/*视频输入流信息*/
	video_input.img_convert_ctx=swsctx;//格式转换上下文
	video_input.in_packet=packet;//数据包
	video_input.pcodecCtx=context;
	video_input.pCodec=video_avcodec;/*保存解码器信息*/
	video_input.v_ifmtCtx=ps;//输入流并读取头部信息
	video_input.videoindex=videostream;/*视频流*/
	video_input.pFrame=frame;/*视频帧*/
	video_input.pFrameYUV=frameyuv;/*申请YUV空间*/

	//水印添加处理
	frameyuv->width=width;
	frameyuv->height=height;
	frameyuv->format=AV_PIX_FMT_YUV420P;
	AVFrame *frame_out=av_frame_alloc();
	unsigned char *frame_buffer_out;
	frame_buffer_out=(unsigned char *)av_malloc(size);
	av_image_fill_arrays(frame_out->data,frame_out->linesize,frame_buffer_out,AV_PIX_FMT_YUV420P,width,height,16);
	/*读帧*/
	char *p=NULL;

	int go=0;
	int Framecount=0;
	time_t sec,sec2;
	char sys_time[64];
	while(video_flag)
	{
		if(!mp4_decode_stat)
		{
			res=av_read_frame(ps,packet);//读取数据
			if(res>=0)
			{
				if(packet->stream_index == AVMEDIA_TYPE_VIDEO)//视频流
				{
					/*解码一帧视频数据。输入一个压缩编码的结构体AVPacket，输出一个解码后的结构体AVFrame*/
					res=avcodec_decode_video2(ps->streams[videostream]->codec,frame,&go,packet);
					if(res<0)
					{
						printf("avcodec_decode_video2 failed\n");
						break;
					}
					if(go)
					{
						/*转换像素的函数*/
						sws_scale(swsctx,(const uint8_t * const*)frame->data,frame->linesize,0,context->height,frameyuv->data,frameyuv->linesize);						
						sec=time(NULL);
						if(sec!=sec2)
						{
							sec2=sec;
							struct tm* today = localtime(&sec2);
							strftime(sys_time, sizeof(sys_time), "%Y/%m/%d %H\\:%M\\:%S", today);  
						}
						//添加水印,调用libavfilter库实现
						waterMark(frameyuv,frame_out,width,height,sys_time);
						//yuv420p，y表示亮度，uv表示像素颜色
						p=frame_buffer_out;
						memcpy(p,frame_out->data[0],frame_out->height*frame_out->width);//y,占用空间w*h
						p+=frame_out->width*frame_out->height;
						memcpy(p,frame_out->data[1],frame_out->height/2*frame_out->width/2);//u,占用空间(w/2)*(h/2)
						p+=frame_out->height/2*frame_out->width/2;
						memcpy(p,frame_out->data[2],frame_out->height/2*frame_out->width/2);//v,占用空间(w/2)*(h/2)
						p+=frame_out->height/2*frame_out->width/2;
		
						pthread_mutex_lock(&fastmutex);//互斥锁上锁
						memcpy(rgb_buff,frame_buffer_out,size);
						pthread_cond_broadcast(&cond);//广播唤醒所有线程
						pthread_mutex_unlock(&fastmutex);//互斥锁解锁
					}
				}
			}
		}
	}
	sws_freeContext(swsctx);/*释放上下文*/
	av_frame_free(&frameyuv);/*释放YUV空间*/
	av_packet_unref(packet);/*释放包*/
	av_frame_free(&frame);/*释放视频帧*/
	avformat_close_input(&ps);/*关闭流*/

	sws_freeContext(video_input.img_convert_ctx);
	avcodec_close(video_input.pcodecCtx);
	av_free(video_input.pFrameYUV);
	av_free(video_input.pFrame);
	avformat_close_input(&video_input.v_ifmtCtx);
	video_flag=0;
	pthread_exit(NULL);	
}
/*MP4格式数据保存*/
void *Video_savemp4(void*arg)
{
	while(1)
	{
		if(mp4_decode_stat)
		{
			int res;
			AVFormatContext *oc=NULL;
			AVDictionary *opt=NULL;
			/* 创建的AVFormatContext结构体。*/
			avformat_alloc_output_context2(&oc,NULL,"mp4",NULL);//通过文件名创建
			if(oc==NULL)
			{
				printf("为输出格式分配AVFormatContext失败\n");
				avformat_alloc_output_context2(&oc,NULL,"mp4",NULL);//通过文件名创建
				return 0;
			}
			if(oc==NULL)return (void*)1;
			/*输出流信息*/
			AVOutputFormat *ofmt=oc->oformat;
			printf("ofmt->video_codec=%d\n",ofmt->video_codec);
			int have_video=1;
			int encode_video=0;
			OutputStream video_st={0};
			if(ofmt->video_codec !=AV_CODEC_ID_NONE)
			{
				/*添加一个输出流*/
				add_stream(&video_st,oc,&video_input.pCodec,ofmt->video_codec);
				have_video=1;
				encode_video=1;
			}
			printf("w=%d,h=%d,size=%d\n",width,height,size);
			/*视频帧处理*/
			if(have_video)open_video(oc,video_input.pCodec,&video_st,opt);
			printf("打开输出文件成功\r\n");
			/*打印有关输入或输出格式信息*/
			av_dump_format(oc, 0,file_name,1);
			if(!(ofmt->flags & AVFMT_NOFILE))
			{
				/*打开输出文件，成功之后创建的AVFormatContext结构体*/
				res=avio_open(&oc->pb,file_name,AVIO_FLAG_WRITE);
				if(res<0)
				{
					printf("%s open failed :%s\n",file_name,av_err2str(res));
					return  (void*)1;
				}
			}
			/*写入流数据头*/
			res=avformat_write_header(oc,&opt);
			if(res<0)
			{
				printf("open output faile:%s\n",av_err2str(res));
				return  (void*)1;
			}
			if(res<0)
			{
				printf("open output faile:%s\n",av_err2str(res));
				return  (void*)1;
			}
			int got_pic;
			while(encode_video)
			{
				/*获取流数据*/
				AVFrame *frame=get_video_frame(&video_st,&video_input,&got_pic);
				if(!got_pic || frame==NULL)
				{
					usleep(10000);
					continue;
				}
				
				encode_video=!write_video_frame(oc,&video_st,frame);
			}
			/*将流数据写入输出媒体文件并释放文件私有数据。*/
			av_write_trailer(oc);
			/*关闭AVIOContext*s访问的资源，释放它，并将指向它的指针设置为NULL*/
			if(!(ofmt->flags & AVFMT_NOFILE))avio_closep(&oc->pb);
			/*释放AVFormatContext及其所有流。*/
			avformat_free_context(oc);
			/*关闭流*/
			if(have_video)close_stream(oc, &video_st);
			mp4_decode_stat=0;
		}
	}
	
}
/*
简单叙述一下ffmpeg编码流程：

          1、首先使用av_register_all()函数注册所有的编码器和复用器(理解为格式封装器)。该步骤必须放在所有ffmpeg代码前第一个执行

          2、avformat_alloc_output_context2()：初始化包含有输出码流(AVStream)和解复用器(AVInputFormat)的AVFormatContext
			int avformat_alloc_output_context2(AVFormatContext **ctx, ff_const59 AVOutputFormat *oformat,
                                   const char *format_name, const char *filename);
				ctx：函数调用成功之后创建的AVFormatContext结构体。
				oformat：指定AVFormatContext中的AVOutputFormat，用于确定输出格式。如果指定为NULL，可以设定后两个参数（format_name或者filename）由FFmpeg猜测输出格式。
				format_name：指定输出格式的名称。根据格式名称，FFmpeg会推测输出格式。输出格式可以是“flv”，“mkv”等等。
				filename：指定输出文件的名称。根据文件名称，FFmpeg会推测输出格式。文件名称可以是“xx.flv”，“yy.mkv”等等。
				函数执行成功的话，其返回值大于等于0。


          3、avio_open( )打开输出文件
		  int avio_open(AVIOContext **s, const char *url, int flags);
		  int avio_open2(AVIOContext **s, const char *url, int flags,const AVIOInterruptCB *int_cb, AVDictionary **options);
			s：函数调用成功之后创建的AVIOContext结构体。
			url：输入输出协议的地址（文件也是一种“广义”的协议，对于文件来说就是文件的路径）。
			flags：打开地址的方式。可以选择只读，只写，或者读写。取值如下。
				AVIO_FLAG_READ：只读。
				AVIO_FLAG_WRITE：只写。
				AVIO_FLAG_READ_WRITE：读写。
		   后两个参数一般填NULL


          4、av_new_stream() 创建视频码流 该函数生成一个空AVstream 该结构存放编码后的视频码流 。视频码流被拆分为AVPacket新式保存在AVStream中。
		  

         5、设置编码器信息，该步骤主要是为AVCodecContext(从AVStream->codec 获取指针)结构体设置一些参数，包括codec_id、codec_type、width、height、pix_fmt .....  根据编码器的不同，还要额外设置一些参数(如 h264 要设置qmax、qmin、qcompress参数才能正常使用h264编码)

         6、查找并打开编码器，根据前一步设置的编码器参数信息，来查找初始化一个编码其，并将其打开。用到函数为av_fine_encoder()和av_open2()。


         7、写头文件  avformat_write_header()。这一步主要是将封装格式的信息写入文件头部位置。
			int avformat_write_header(AVFormatContext *s, AVDictionary **options);
				s：用于输出的AVFormatContext。
				options：额外的选项，一般为NULL
				函数正常执行后返回值等于0。

         8、编码帧。用到的函数 avcodec_encode_video2() 将AVFrame编码为AVPacket

         9、在写入文件之前 还需要做一件事情就是设置AVPacket一些信息。这些信息关乎最后封装格式能否被正确读取。后面回详细讲述该部分内容

        10、编码帧写入文件 av_write_frame()

        11、flush_encoder()：输入的像素数据读取完成后调用此函数。用于输出编码器中剩余的AVPacket。

        12、av_write_trailer()：写文件尾（对于某些没有文件头的封装格式，不需要此函数。比如说MPEG2TS）。
*/
