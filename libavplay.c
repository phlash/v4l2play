#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <libavformat/avformat.h>
#include <SDL.h>

int barf(char *msg, int rv) {
    int i;
    fprintf(stderr, "\n%s(0x%08x): ", msg, rv);
    // assumes rv is -AVERROR_xxx (it usually is)
    // see ~include/libavutil/error.h and ~include/libavutil/common.h
    // for encoding mechanism we are reversing here..
    rv = -rv;
    for (i=0; i<4; i++) {
        char c = (char)(rv&0x7f);
        fprintf(stderr, "%c", isprint(c) ? c : '.');
        rv >>= 8;
    }
    fprintf(stderr, "\n");
    return 1;
}

static int done = 0;
void trap(int sig) {
    done = 1;
}

static AVFormatContext *g_in_context;
static AVCodecContext *g_dec0, *g_dec1;
static AVFrame *g_frm0, *g_frm1;
void nuke_allocs() {
    if (g_frm0)
        av_frame_free(&g_frm0);
    if (g_frm1)
        av_frame_free(&g_frm1);
    if (g_dec0)
        avcodec_free_context(&g_dec0);
    if (g_dec1)
        avcodec_free_context(&g_dec1);
    if (g_in_context)
        avformat_close_input(&g_in_context);
}
void nuke_network() {
    avformat_network_deinit();
}

Uint32 map_format(enum AVPixelFormat pix_fmt) {
    Uint32 rv = SDL_PIXELFORMAT_UNKNOWN;
    switch(pix_fmt) {
    case AV_PIX_FMT_YUV420P:
        rv = SDL_PIXELFORMAT_YV12;  // guessing as there is f*ck all documentation..*sigh*
        break;
    default:
        barf("unknown pixel format", 0);
    }
    return rv;
}

void render_frame(SDL_Renderer *rnd, SDL_Texture *txt, AVFrame *frm) {
    Uint32 fmt;
    int acc, w, h;
    unsigned char *pixels;
    int pitch;
    if (SDL_QueryTexture(txt, &fmt, &acc, &w, &h)) {
        barf("reading texture properties", 0);
        return;
    }
    if (SDL_LockTexture(txt, NULL, (void **)&pixels, &pitch)) {
        barf("unable to lock texture", 0);
        return;
    }
    if (pitch==frm->linesize[0]) {
        // matching image size expectation, we can memcpy the whole thing
        // (while swapping U&V blocks to convert YUV420P -> YV12 we hope)
        int size = pitch * h;
        memcpy(pixels, frm->data[0], size);           // Y
        memcpy(pixels+size, frm->data[2], size/4);    // V
        memcpy(pixels+size*5/4, frm->data[1], size/4);// U
    } else if (pitch<frm->linesize[0]) {
        // pitch lower than frame size (padding most likely), so we work
        // line-by-line, skipping ends of frame lines, still swapping U&V
        // NB: we must actually work in two-line increments as there is
        // a single U or V line for each pair of Y lines.
        int size = pitch * h;
        int i;
        for (i=0; i<h/2; ++i) {
            memcpy(pixels+2*i*pitch, frm->data[0]+2*i*frm->linesize[0], pitch);         // Y
            memcpy(pixels+(2*i+1)*pitch, frm->data[0]+(2*i+1)*frm->linesize[0], pitch); // Y+1
            memcpy(pixels+size+i*pitch/2, frm->data[2]+i*frm->linesize[2], pitch/2);    // V
            memcpy(pixels+size*5/4+i*pitch/2, frm->data[1]+i*frm->linesize[1], pitch/2);// U
        }
    } else {
        barf("image size mismatch", (pitch<<16) | frm->linesize[0]);
    }
    SDL_UnlockTexture(txt);
    SDL_RenderCopy(rnd, txt, NULL, NULL);
    SDL_RenderPresent(rnd);
}

int decode_packet(SDL_Renderer *rnd, SDL_Texture *txt, AVCodecContext *ctx, AVFrame *frm, AVPacket *pkt) {
    int rv, cnt;
    if ((rv=avcodec_send_packet(ctx, pkt))>=0) {
        // loop to handle multi-frame packets
        for (cnt=0; (rv=avcodec_receive_frame(ctx, frm))>=0; ++cnt) {
            // write video frame to screen
            if (AVMEDIA_TYPE_VIDEO==ctx->codec_type) {
                render_frame(rnd, txt, frm);
            }
        }
    }
    // end of multi-frame packet is one of these, return counter
    // invalid data can happen too.. ignore
    if (AVERROR_INVALIDDATA==rv || AVERROR_EOF==rv || AVERROR(EAGAIN)==rv)
        rv = cnt;
    return rv;
}

int main(int argc, char **argv) {
    char *url = "rtsp://192.168.0.150:8080/video/h264";
    int rv = 0, pkts = 0;
    int64_t total, s0, s1, us;
    AVCodec *c0, *c1;
    AVCodecContext *vCtx, *aCtx;
    AVPacket *packet;
    SDL_Window *win;
    SDL_Renderer *rnd;
    SDL_Texture *txt;
    char *msg;
    if (argc>1)
        url = argv[1];
    // init SDL for rendering
    if (SDL_Init(SDL_INIT_AUDIO|SDL_INIT_VIDEO)!=0)
        return 1;
    atexit(SDL_Quit);
    // dump info
    unsigned int ver = avformat_version();
    printf("avformat:version: %d.%d.%d\n", ver>>16, (ver>>8)&0xff, ver&0xff);
    // apparantly we need to init some network guff
    avformat_network_init();
    atexit(nuke_network);
    // go get that stream!
    rv = avformat_open_input(&g_in_context, url, NULL, NULL);
    if (rv<0)
        return barf("opening url stream", rv);
    atexit(nuke_allocs);
    // determine content types
    rv = avformat_find_stream_info(g_in_context, NULL);
    if (rv<0)
        return barf("reading stream info", rv);
    // dump info
    av_dump_format(g_in_context, 0, url, 0);
    if (g_in_context->nb_streams != 2)
        return barf("expecting 2 sub-streams", g_in_context->nb_streams);
    // find decoders
    c0 = avcodec_find_decoder(g_in_context->streams[0]->codecpar->codec_id);
    c1 = avcodec_find_decoder(g_in_context->streams[1]->codecpar->codec_id);
    if (!c0 || !c1)
        return barf("unable to find codecs", 0);
    printf("stream#0: %s\nstream#1: %s\n", c0->name, c1->name);
    // allocate contexts
    g_dec0 = avcodec_alloc_context3(c0);
    g_dec1 = avcodec_alloc_context3(c1);
    if (!g_dec0 || !g_dec1)
        return barf("unable to alloc codec contexts", 0);
    vCtx = AVMEDIA_TYPE_VIDEO==g_dec0->codec_type ? g_dec0 : g_dec1;
    aCtx = AVMEDIA_TYPE_VIDEO==g_dec0->codec_type ? g_dec1 : g_dec0;
    // transfer other paremters (if any)
    if ((rv=avcodec_parameters_to_context(g_dec0, g_in_context->streams[0]->codecpar)) ||
        (rv=avcodec_parameters_to_context(g_dec1, g_in_context->streams[1]->codecpar)))
        return barf("transfering codec parametesr", rv);
    // open contexts
    if ((rv=avcodec_open2(g_dec0, c0, NULL))<0 ||
        (rv=avcodec_open2(g_dec1, c1, NULL))<0)
        return barf("opening codec contexts", rv);
    // allocate decoded frames
    g_frm0 = av_frame_alloc();
    g_frm1 = av_frame_alloc();
    if (!g_frm0 || !g_frm1)
        return barf("unable to alloc frames", 0);
    // Create output window
    win = SDL_CreateWindow("ooh look! an AV stream!",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        vCtx->width, vCtx->height, 0);
    rnd = SDL_CreateRenderer(win, -1, 0);
    txt = SDL_CreateTexture(rnd, map_format(vCtx->pix_fmt),
        SDL_TEXTUREACCESS_STREAMING,
        vCtx->width, vCtx->height);
    if (!win || !rnd || !txt)
        return barf("creating output window", 0);
    // grab frames (finally!)
    packet = av_packet_alloc();
    if (!packet)
        return barf("allocating input packet", rv);
    signal(SIGINT, trap);
    total = s0 = s1 = us = 0;
    msg = "reading packet";
    while ((rv=av_read_frame(g_in_context, packet))==0) {
        SDL_Event ev;
        // feed the decoder(s)
        total += packet->size;
        if (0==packet->stream_index) {
            rv = decode_packet(rnd, txt, g_dec0, g_frm0, packet);
            s0 += rv;
        } else if(1==packet->stream_index) {
            rv = decode_packet(rnd, txt, g_dec1, g_frm1, packet);
            s1 += rv;
        } else {
            ++us;
        }
        if (rv<0) {
            msg = "decoding packet";
            break;
        }
        msg = "reading packet";
        printf("\rpackets:%04d, bytes:%ld, s0frms:%ld, s1frms:%ld, us:%ld   ", pkts, total, s0, s1, us);
        fflush(stdout);
        ++pkts;
        if (done)
            break;
        // pump the SDL queue
        while (SDL_PollEvent(&ev)) {
            if (SDL_QUIT==ev.type)
                done = 1;
        }
    }
    if (rv<0)
        barf(msg, rv);
    else
        printf("clean exit\n");
    // done
    return rv;
}
