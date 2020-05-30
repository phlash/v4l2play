#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>
#include <libavformat/avformat.h>
#include <SDL.h>
#include <execinfo.h>

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
void *oops[20];
void trap(int sig) {
    if (SIGSEGV==sig) {
        int n=backtrace(oops, 20);
        backtrace_symbols_fd(oops, n, 2);
        _exit(1);
    }
    done = 1;
}

static AVFormatContext *g_in_context;
static AVCodecContext *g_dec[2];
static AVFrame *g_frm[2];
void nuke_allocs() {
    if (g_frm[0])
        av_frame_free(&g_frm[0]);
    if (g_frm[1])
        av_frame_free(&g_frm[1]);
    if (g_dec[0])
        avcodec_free_context(&g_dec[0]);
    if (g_dec[1])
        avcodec_free_context(&g_dec[1]);
    if (g_in_context)
        avformat_close_input(&g_in_context);
}
void nuke_network() {
    avformat_network_deinit();
}

Uint32 map_vformat(enum AVPixelFormat pix_fmt) {
    Uint32 rv = SDL_PIXELFORMAT_UNKNOWN;
    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:
        rv = SDL_PIXELFORMAT_YV12;  // guessing as there is f*ck all documentation..*sigh*
        break;
    default:
        barf("unknown pixel format", pix_fmt);
    }
    return rv;
}

SDL_AudioFormat map_aformat(enum AVSampleFormat aud_fmt) {
    SDL_AudioFormat rv = 0;
    switch (aud_fmt) {
    case AV_SAMPLE_FMT_FLTP:
        rv = AUDIO_F32SYS;
        break;
    default:
        barf("unknown audio format", aud_fmt);
    }
    return rv;
}

static struct timespec g_last;
static float g_vr;
void render_video(SDL_Renderer *rnd, SDL_Texture *txt, AVFrame *frm) {
    Uint32 fmt;
    int acc, w, h;
    unsigned char *pixels;
    int pitch;
    if (g_last.tv_sec!=0) {
        // frame>0, delay to achieve specified video frame rate
        long ns, nx;
        ns = 1000000000L/(long)g_vr;
        nx = g_last.tv_nsec+ns;
        g_last.tv_nsec = nx%1000000000;
        g_last.tv_sec = g_last.tv_sec+(nx/1000000000);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &g_last, NULL);
    } else {
        // frame 0, grab entry time
        clock_gettime(CLOCK_MONOTONIC, &g_last);
    }
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

void render_audio(SDL_AudioDeviceID adv, AVFrame *frm) {
    // interleaving buffer
    #define AUD_CHUNK_SIZE  1024
    float intbuf[AUD_CHUNK_SIZE];
    int n, i, c, nc = av_get_channel_layout_nb_channels(frm->channel_layout);
    // check we have FLTP format incoming..
    if (frm->format != AV_SAMPLE_FMT_FLTP) {
        barf("unknown audio format", frm->format);
        return;
    }
    // convert to interleaved format and queue for SDL in chunks
    n = 0;
    for (i=0; i<frm->nb_samples; ++i) {
        for (c=0; c<nc; ++c) {
            if (n>=AUD_CHUNK_SIZE) {
                SDL_QueueAudio(adv, intbuf, n*sizeof(float));
                n = 0;
            }
            intbuf[n++] = ((float*)frm->extended_data[c])[i];
        }
    }
    if (n>0)
        SDL_QueueAudio(adv, intbuf, n*sizeof(float));
    // make sure we're running..
    SDL_PauseAudioDevice(adv, 0);
}

int decode_packet(SDL_Renderer *rnd, SDL_Texture *txt, SDL_AudioDeviceID adv, AVPacket *pkt) {
    int rv, cnt;
    if ((rv=avcodec_send_packet(g_dec[pkt->stream_index], pkt))>=0) {
        // loop to handle multi-frame packets
        for (cnt=0;
            (rv=avcodec_receive_frame(g_dec[pkt->stream_index],
            g_frm[pkt->stream_index]))>=0;
            ++cnt) {
            // write video frame to screen
            if (AVMEDIA_TYPE_VIDEO==g_dec[pkt->stream_index]->codec_type) {
                render_video(rnd, txt, g_frm[pkt->stream_index]);
            // write audio frame to sound device
            } else if(AVMEDIA_TYPE_AUDIO==g_dec[pkt->stream_index]->codec_type) {
                render_audio(adv, g_frm[pkt->stream_index]);
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
    int64_t total, us, sc[2];
    AVCodec *c0, *c1;
    int vidx, ridx;
    AVPacket *packet;
    float r0, r1, vr;
    SDL_Window *win;
    SDL_Renderer *rnd;
    SDL_Texture *txt;
    SDL_AudioDeviceID adv;
    SDL_AudioSpec wnt, hve;
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
    if (g_in_context->nb_streams != 2)
        return barf("expecting 2 sub-streams", g_in_context->nb_streams);
    // find decoders
    c0 = avcodec_find_decoder(g_in_context->streams[0]->codecpar->codec_id);
    c1 = avcodec_find_decoder(g_in_context->streams[1]->codecpar->codec_id);
    if (!c0 || !c1)
        return barf("unable to find codecs", 0);
    // find video stream and save rate
    vidx = AVMEDIA_TYPE_VIDEO==c0->type ? 0 : 1;
    g_vr = (float)g_in_context->streams[vidx]->avg_frame_rate.num/(float)g_in_context->streams[vidx]->avg_frame_rate.den;
    // dump info
    printf("stream#0: %s\nstream#1: %s\n", c0->name, c1->name);
    printf("vidx: %d@%.1ffps\n", vidx, g_vr);
    if (getenv("LIBAVPLAY_DUMP"))
        av_dump_format(g_in_context, 0, url, 0);
    // allocate contexts
    g_dec[0] = avcodec_alloc_context3(c0);
    g_dec[1] = avcodec_alloc_context3(c1);
    if (!g_dec[0] || !g_dec[1])
        return barf("unable to alloc codec contexts", 0);
    // transfer other paremters (if any)
    if ((rv=avcodec_parameters_to_context(g_dec[0], g_in_context->streams[0]->codecpar)) ||
        (rv=avcodec_parameters_to_context(g_dec[1], g_in_context->streams[1]->codecpar)))
        return barf("transfering codec parametesr", rv);
    // open contexts
    if ((rv=avcodec_open2(g_dec[0], c0, NULL))<0 ||
        (rv=avcodec_open2(g_dec[1], c1, NULL))<0)
        return barf("opening codec contexts", rv);
    // allocate decoded frames
    g_frm[0] = av_frame_alloc();
    g_frm[1] = av_frame_alloc();
    if (!g_frm[0] || !g_frm[1])
        return barf("unable to alloc frames", 0);
    // create output window
    win = SDL_CreateWindow("ooh look! an AV stream!",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_dec[vidx]->width, g_dec[vidx]->height, 0);
    rnd = SDL_CreateRenderer(win, -1, 0);
    txt = SDL_CreateTexture(rnd, map_vformat(g_dec[vidx]->pix_fmt),
        SDL_TEXTUREACCESS_STREAMING,
        g_dec[vidx]->width, g_dec[vidx]->height);
    if (!win || !rnd || !txt)
        return barf("creating output window", 0);
    // create output audio
    memset(&wnt, 0, sizeof(wnt));
    memset(&hve, 0, sizeof(hve));
    wnt.freq = g_dec[1-vidx]->sample_rate;
    wnt.channels = g_dec[1-vidx]->channels;
    wnt.samples = g_dec[1-vidx]->frame_size;
    wnt.format = map_aformat(g_dec[1-vidx]->sample_fmt);
    adv = SDL_OpenAudioDevice(NULL, 0, &wnt, &hve, 0);
    if (!adv)
        return barf("opening audio output", 0);
    printf("aidx: %d@$%dHz %dch\n", 1-vidx, hve.freq, hve.channels);
    // grab frames (finally!)
    packet = av_packet_alloc();
    if (!packet)
        return barf("allocating input packet", rv);
    signal(SIGINT, trap);
    signal(SIGSEGV, trap);
    total = sc[0] = sc[1] = us = 0;
    msg = "reading packet";
    while ((rv=av_read_frame(g_in_context, packet))==0) {
        SDL_Event ev;
        // feed the decoder(s)
        total += packet->size;
        if (packet->stream_index < 2) {
            rv = decode_packet(rnd, txt, adv, packet);
            sc[packet->stream_index] += rv;
        } else {
            ++us;
        }
        if (rv<0) {
            msg = "decoding packet";
            break;
        }
        msg = "reading packet";
        printf("\rpackets:%04d, bytes:%ld, vfrms:%ld, afrms:%ld, us:%ld   ", pkts, total, sc[vidx], sc[1-vidx], us);
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
