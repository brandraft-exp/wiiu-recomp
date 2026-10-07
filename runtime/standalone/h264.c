/* Cafe decoder ABI follows Cemu H264Dec.cpp::H264DEC* and
 * H264DoFrameOutputCallback; NV12 row alignment follows H264DecBackendAVC.
 * Decoding uses libavcodec with guest timestamps/output buffers tracked through
 * frame reordering. No title-specific movie or image data is embedded here. */
#include "rt.h"
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#define INVALID 0x1010000u
#define BAD_STREAM 0x1000000u
typedef struct pending {int64_t id;uint64_t timestamp;uint32_t image;struct pending *next;} pending;
typedef struct result {uint32_t words[30];struct result *next;} result;
typedef struct decoder {
    uint32_t address,callback,user,stream,length,scratch;
    uint64_t timestamp;int per_frame,inflight;int64_t serial;
    AVCodecContext *codec;AVFrame *decoded;struct SwsContext *convert;
    pending *pending;result *ready,*tail;
    struct decoder *next;
} decoder;
static decoder *decoders;
static decoder *find(uint32_t address){for(decoder *d=decoders;d;d=d->next)if(d->address==address)return d;return NULL;}
static void reset(decoder *d){
    av_frame_free(&d->decoded);avcodec_free_context(&d->codec);sws_freeContext(d->convert);d->convert=NULL;
    while(d->pending){pending *p=d->pending;d->pending=p->next;free(p);}
    while(d->ready){result *p=d->ready;d->ready=p->next;free(p);}d->tail=NULL;d->inflight=0;
}
static void requirement(ppc_ctx *c){
    uint32_t profile=c->r[3],level=c->r[4],w=c->r[5],h=c->r[6],out=c->r[7];
    if(w<32||h<32||w>2800||h>1408||!out||level>=52||(profile!=66&&profile!=77&&profile!=100)){c->r[3]=INVALID;return;}
    uint32_t n=level<11?0x18c:level==11?0x384:level<=20?0x948:level==21?0x1290:level<=30?0x1fa4:level==31?0x4650:level==32?0x5000:level<=41?0x8000:level==42?0x8800:level<=50?0x1af40:0x2d000;
    st32(out,(n<<10)+0x447);c->r[3]=0;
}
static void init(ppc_ctx *c){
    if(c->r[3]<64||!c->r[4]){c->r[3]=INVALID;return;}
    decoder *d=find(c->r[4]);if(d)reset(d);else{d=calloc(1,sizeof *d);if(!d)rt_fatal(c,"allocating H264 decoder");d->address=c->r[4];d->next=decoders;decoders=d;}
    d->callback=d->user=d->per_frame=0;memset(g_mem+d->address,0,64);c->r[3]=0;
}
/* The Cafe per-frame mode delivers in decode order. FFmpeg's normal output
 * waits for presentation-order reordering even with LOW_DELAY. Its public
 * draw_horiz_band hook exposes the completed coded frame without flushing DPB.
 * See FFmpeg h264dec.c::ff_h264_draw_horiz_band. */
static void decoded_band(AVCodecContext *ctx,const AVFrame *frame,int offset[AV_NUM_DATA_POINTERS],int y,int type,int height){
    (void)offset;(void)type;
    if(y+height<ctx->height)return;
    decoder *d=ctx->opaque;av_frame_free(&d->decoded);d->decoded=av_frame_clone(frame);
    if(!d->decoded)abort();
}
static void open_decoder(ppc_ctx *c){
    decoder *d=find(c->r[3]);if(!d){c->r[3]=INVALID;return;}reset(d);
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);d->codec=avcodec_alloc_context3(codec);
    if(!d->codec)rt_fatal(c,"allocating H264 codec");d->codec->thread_count=1;d->codec->apply_cropping=0;
    if(d->per_frame){d->codec->opaque=d;d->codec->draw_horiz_band=decoded_band;d->codec->slice_flags=SLICE_FLAG_CODED_ORDER;}
    c->r[3]=avcodec_open2(d->codec,codec,NULL)<0?BAD_STREAM:0;
}
static void close_decoder(ppc_ctx *c){decoder *d=find(c->r[3]);if(d)reset(d);c->r[3]=0;}
static void begin(ppc_ctx *c){decoder *d=find(c->r[3]);if(!d||!d->codec){c->r[3]=INVALID;return;}d->codec->opaque=d;d->codec->draw_horiz_band=d->per_frame?decoded_band:NULL;d->codec->slice_flags=SLICE_FLAG_CODED_ORDER;avcodec_flush_buffers(d->codec);c->r[3]=0;}
static void set_callback(ppc_ctx *c){decoder *d=find(c->r[3]);if(!d){c->r[3]=INVALID;return;}d->callback=c->r[4];c->r[3]=0;}
static void set_user(ppc_ctx *c){decoder *d=find(c->r[3]);if(!d){c->r[3]=INVALID;return;}d->user=ld32(c->r[4]);c->r[3]=0;}
static void set_per_frame(ppc_ctx *c){decoder *d=find(c->r[3]);if(!d){c->r[3]=INVALID;return;}d->per_frame=c->r[4]!=0;c->r[3]=0;}
static void bitstream(ppc_ctx *c){decoder *d=find(c->r[3]);if(!d||(uint64_t)c->r[4]+c->r[5]>(1ull<<32)){c->r[3]=INVALID;return;}d->stream=c->r[4];d->length=c->r[5];d->timestamp=c->f[1];c->r[3]=0;}
static int convert_frame(decoder *d,const AVFrame *f){
        pending **link=&d->pending;while(*link&&(*link)->id!=f->pts)link=&(*link)->next;
        if(!*link){return BAD_STREAM;}pending *p=*link;*link=p->next;
        if(getenv("WIIU_TRACE_VIDEO")&&(p->id<=4||p->id%30==0)){
            double timestamp;memcpy(&timestamp,&p->timestamp,8);
            fprintf(stderr,"H264 output work=%08x frame=%lld timestamp=%.6f image=%08x %dx%d\n",d->address,(long long)p->id,timestamp,p->image,f->width,f->height);
        }
        unsigned stride=(f->width+255)&~255u;uint64_t size=(uint64_t)stride*f->height*3/2;
        if(f->width<=0||f->height<=0||!p->image||p->image+size>(1ull<<32)){free(p);return INVALID;}
        d->convert=sws_getCachedContext(d->convert,f->width,f->height,f->format,f->width,f->height,AV_PIX_FMT_NV12,SWS_POINT,NULL,NULL,NULL);
        if(!d->convert){free(p);return BAD_STREAM;}
        uint8_t *planes[4]={g_mem+p->image,g_mem+p->image+stride*f->height,NULL,NULL};int lines[4]={(int)stride,(int)stride,0,0};
        sws_scale(d->convert,(const uint8_t *const*)f->data,f->linesize,0,f->height,planes,lines);
        // The decoder writes reusable video buffers directly. Notify the GPU
        // cache before exposing the completed frame to the guest callback.
        hle_gpu_memory_written(p->image,(uint32_t)size);
        result *r=calloc(1,sizeof *r);if(!r)abort();r->words[0]=100;r->words[2]=p->timestamp>>32;r->words[3]=p->timestamp;
        r->words[4]=f->width;r->words[5]=f->height;r->words[6]=stride;
        r->words[7]=(f->crop_top|f->crop_bottom|f->crop_left|f->crop_right)!=0;
        r->words[8]=f->crop_top;r->words[9]=f->crop_bottom;r->words[10]=f->crop_left;r->words[11]=f->crop_right;r->words[17]=p->image;
        if(d->tail)d->tail->next=r;else d->ready=r;d->tail=r;free(p);
    return 0;
}
static int receive(decoder *d){
    AVFrame *f=av_frame_alloc();if(!f)return BAD_STREAM;int status;
    while((status=avcodec_receive_frame(d->codec,f))==0){
        if(!d->per_frame){int error=convert_frame(d,f);if(error){av_frame_free(&f);return error;}}
        av_frame_unref(f);
    }
    av_frame_free(&f);
    if(d->per_frame&&d->decoded){int error=convert_frame(d,d->decoded);av_frame_free(&d->decoded);if(error)return error;}
    return status==AVERROR(EAGAIN)||status==AVERROR_EOF?0:BAD_STREAM;
}

static void output(ppc_ctx *c,decoder *d){
    result *r=d->ready;d->ready=r->next;if(!d->ready)d->tail=NULL;d->inflight--;
    if(d->callback){
        if(!d->scratch)d->scratch=hle_sys_alloc(0x100);uint32_t a=d->scratch;
        for(unsigned i=0;i<30;i++)st32(a+32+i*4,r->words[i]);st32(a,1);st32(a+4,a+16);st32(a+8,d->user);st32(a+16,a+32);
        ppc_ctx saved=*c;c->r[3]=a;ppc_call_indirect(c,d->callback);*c=saved;
    }free(r);
}
static void execute(ppc_ctx *c){
    decoder *d=find(c->r[3]);if(!d||!d->codec||!d->length||d->length>INT32_MAX){c->r[3]=INVALID;return;}
    AVPacket *packet=av_packet_alloc();if(!packet||av_new_packet(packet,d->length)<0)rt_fatal(c,"allocating H264 packet");
    memcpy(packet->data,g_mem+d->stream,d->length);packet->pts=++d->serial;
    pending *p=calloc(1,sizeof *p);if(!p)rt_fatal(c,"allocating H264 submission");p->id=packet->pts;p->timestamp=d->timestamp;p->image=c->r[4];p->next=d->pending;d->pending=p;
    int error=avcodec_send_packet(d->codec,packet);av_packet_free(&packet);
    if(error<0){if(getenv("WIIU_TRACE_VIDEO"))fprintf(stderr,"H264 submit error %d\n",error);d->pending=p->next;free(p);c->r[3]=BAD_STREAM;return;}d->inflight++;
    error=receive(d);if(error){if(getenv("WIIU_TRACE_VIDEO"))fprintf(stderr,"H264 receive error %x frame=%lld\n",error,(long long)d->serial);c->r[3]=error;return;}
    if(d->ready&&d->inflight>(d->per_frame?0:5))output(c,d);
    c->r[3]=0xe4;
}
static void end(ppc_ctx *c){decoder *d=find(c->r[3]);if(!d||!d->codec){c->r[3]=INVALID;return;}avcodec_send_packet(d->codec,NULL);int error=receive(d);while(d->ready)output(c,d);c->r[3]=error;}
static void check_memory(ppc_ctx *c){c->r[3]=(uint64_t)c->r[3]+c->r[4]>(1ull<<32);}
hle_fn hle_h264_lookup(const char *name){
#define E(n,f) if(!strcmp(name,n))return f
    E("H264DECMemoryRequirement",requirement);E("H264DECInitParam",init);E("H264DECOpen",open_decoder);E("H264DECClose",close_decoder);
    E("H264DECBegin",begin);E("H264DECEnd",end);E("H264DECSetParam_FPTR_OUTPUT",set_callback);E("H264DECSetParam_USER_MEMORY",set_user);
    E("H264DECSetParam_OUTPUT_PER_FRAME",set_per_frame);E("H264DECSetBitstream",bitstream);E("H264DECExecute",execute);E("H264DECCheckMemSegmentation",check_memory);
#undef E
    return NULL;
}
