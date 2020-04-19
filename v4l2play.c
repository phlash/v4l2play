// Messin' with video4linux loopback device & external MJPEG sources (cameras)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <linux/videodev2.h>

int barf(char *msg) {
    perror(msg);
    return 1;
}

void print_format(struct v4l2_format* vid_format) {
    fprintf(stderr, "  vid_format->type                =%d\n",	vid_format->type );
    fprintf(stderr, "  vid_format->fmt.pix.width       =%d\n",	vid_format->fmt.pix.width );
    fprintf(stderr, "  vid_format->fmt.pix.height      =%d\n",	vid_format->fmt.pix.height );
    fprintf(stderr, "  vid_format->fmt.pix.pixelformat =0x%08X\n",	vid_format->fmt.pix.pixelformat);
    fprintf(stderr, "  vid_format->fmt.pix.sizeimage   =%d\n",	vid_format->fmt.pix.sizeimage );
    fprintf(stderr, "  vid_format->fmt.pix.field       =%d\n",	vid_format->fmt.pix.field );
    fprintf(stderr, "  vid_format->fmt.pix.bytesperline=%d\n",	vid_format->fmt.pix.bytesperline );
    fprintf(stderr, "  vid_format->fmt.pix.colorspace  =%d\n",	vid_format->fmt.pix.colorspace );
}

char *load(char *image, size_t *psize) {
    FILE *ifile = fopen(image, "rb");
    char *ibuf = NULL;
    if ((NULL == ifile) ||
        fseek(ifile, 0, SEEK_END)<0 ||
        (*psize = (size_t)ftell(ifile))<0 ||
        (ibuf = malloc(*psize)) == NULL ||
        fseek(ifile, 0, SEEK_SET)<0 ||
        fread(ibuf, *psize, 1, ifile)!=1)
            barf("reading test image");
    if (ifile) fclose(ifile);
    return ibuf;
}

FILE *openUrl(char *url) {
    char *host, *port, *path;
    struct sockaddr_in addr;
    int sock, tmp;
    FILE *fp;
    // let's find the markers..
    host = strstr(url, "//");
    host = host ? host+2 : NULL;
    port = host ? strchr(host, ':') : NULL;
    port = port ? port+1 : NULL;
    path = port ? strchr(port, '/') : NULL;
    if (!host || !port || !path) {
        errno = EINVAL;
        barf("failed to parse URL");
        return NULL;
    }
    // cut up the text..
    *(port-1) = 0;
    path[0] = 0;
    path = path+1;
    // parse host as IP address, or lookup as name, then parse port
    if (inet_aton(host, &addr.sin_addr)!=1) {
        struct hostent *ent = gethostbyname(host);
        if (!ent) {
            barf("failed to lookup host");
            return NULL;
        }
        addr.sin_addr = *((struct in_addr *)(ent->h_addr));
    }
    if (sscanf(port, "%d", &tmp)!=1) {
        errno = EINVAL;
        barf("failed to parse port");
        return NULL;
    }
    addr.sin_port = htons((short)tmp);
    addr.sin_family = AF_INET;
    // connect a socket..
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock<0) {
        barf("failed to open socket");
        return NULL;
    }
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr))<0) {
        barf("failed to connect");
        return NULL;
    }
    fp = fdopen(sock, "rb+");
    if (!fp) {
        barf("opening stream");
        return NULL;
    }
    fprintf(fp, "GET /%s HTTP/1.0\r\n", path);
    fprintf(fp, "Host: %s\r\n\r\n", host);
    fflush(fp);
    return fp;
}

// https://stackoverflow.com/questions/4801933/how-to-parse-mjpeg-http-stream-within-c
// Yukky (but robust!) hack to simply look for start/end markers.. 0xff 0xd8 {jpg bytes} 0xff 0xd9
#define MAX_JPG 1048576
#define RJS_NON 0
#define RJS_FF1 1
#define RJS_JPG 2
#define RJS_FF2 3
#define RJS_FIN 4

char *readJpg(FILE *mjpeg, size_t *plen) {
    static char jpg[MAX_JPG];
    int state = RJS_NON;
    *plen = 0;
    do {
        int byte = fgetc(mjpeg);
        if (EOF == byte)
            return NULL;
        switch (state) {
        case RJS_NON:
            if (0xff==byte)
                state = RJS_FF1;
            break;
        case RJS_FF1:
            if (0xd8==byte) {
                state = RJS_JPG;
                jpg[0] = 0xff;
                jpg[1] = 0xd8;
                *plen = 2;
            } else {
                state = RJS_NON;
            }
            break;
        case RJS_JPG:
            jpg[*plen] = (char)byte;
            *plen += 1;
            if (*plen >= MAX_JPG) {
                errno = ERANGE;
                return NULL;
            }
            if (0xff==byte)
                state = RJS_FF2;
            break;
        case RJS_FF2:
            jpg[*plen] = (char)byte;
            *plen += 1;
            if (*plen >= MAX_JPG) {
                errno = ERANGE;
                return NULL;
            }
            if (0xd9==byte)
                state = RJS_FIN;
            else
                state = RJS_JPG;
            break;
        }
    } while (state != RJS_FIN);
    return jpg;
}

static volatile int done = 0;
void sigtrap(int sig) {
    done = 1;
}

int main(int argc, char **argv) {
    char *device = "/dev/video2";
    char *img1 = "test.jpg";
    char *img2 = "flip.jpg";
    char *url = NULL;
    int width = 958;
    int height= 478;
    int rate = 25;
    int arg;
    struct v4l2_capability caps;
    struct v4l2_format fmt;
    FILE *mjpeg;
    size_t simg1, simg2;
    char *bimg1, *bimg2;
    struct timespec now;
    long nsecs, nlast;
    for (arg=1; arg<argc; arg++) {
        if (strncmp(argv[arg], "-?", 2)==0)
            return fprintf(stderr, "usage: v4l2play [-d <device: /dev/video2>] [-w <width: 958>] [-h <height: 478>] [-u <url: none>] [-1|2 <image1|2: test.jpg|flip.jpg>] [-f <rate: 25>]\n");
        else if (strncmp(argv[arg], "-w", 2)==0)
            sscanf(argv[++arg], "%d", &width);
        else if (strncmp(argv[arg], "-h", 2)==0)
            sscanf(argv[++arg], "%d", &height);
        else if (strncmp(argv[arg], "-r", 2)==0)
            sscanf(argv[++arg], "%d", &rate);
        else if (strncmp(argv[arg], "-d", 2)==0)
            device = argv[++arg];
        else if (strncmp(argv[arg], "-u", 2)==0)
            url = argv[++arg];
        else if (strncmp(argv[arg], "-1", 2)==0)
            img1 = argv[++arg];
        else if (strncmp(argv[arg], "-2", 2)==0)
            img2 = argv[++arg];
    }
    fprintf(stderr, "device: %s, width: %d, height: %d, url: %s, img1: %s, img2: %s, rate: %d\n", device, width, height, url, img1, img2, rate);
    // Open Url if provided
    if (url) {
        mjpeg = openUrl(url);
        if (!mjpeg)
            return 1;
    } else {
        // otherwise open test images
        bimg1 = load(img1, &simg1);
        bimg2 = load(img2, &simg2);
        if (NULL==bimg1 || NULL==bimg2)
            return 1;
    }
    // Open and configure loopback driver
    int vfd = open(device, O_RDWR);
    if (vfd < 0)
        return barf("opening device");
    if (ioctl(vfd, VIDIOC_QUERYCAP, &caps)<0)
        return barf("ioctl VIDIOC_QUERYCAPS");
    fprintf(stderr, "driver: %s\n", caps.driver);
    fprintf(stderr, "card  : %s\n", caps.card);
    fprintf(stderr, "caps  : 0x%08X\n", caps.capabilities);
    fprintf(stderr, "devcap: 0x%08X\n", caps.device_caps);
    // Ask for capture format (if set)
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(vfd, VIDIOC_G_FMT, &fmt)<0)
        fprintf(stderr, "  no capture format yet\n");
    else {
        print_format(&fmt);
        goto done;
    }
    // Set our format (MJPEG from the remote camera - why change format here when consumer will anyhow?)
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height= height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.sizeimage = 0; // MJPEG not fixed size
    fmt.fmt.pix.bytesperline = 0; // MJPEG not fixed size
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
    if (ioctl(vfd, VIDIOC_S_FMT, &fmt)<0)
        return barf("ioctl VIDIOC_S_FMT");
    fprintf(stderr, "output format:\n");
    print_format(&fmt);
    // Chuck frames
    signal(SIGINT, sigtrap);
    fprintf(stderr, "Streaming frames, hit Ctrl-C to stop\n");
    nsecs = 1000000000L/(long)rate;
    clock_gettime(CLOCK_MONOTONIC, &now);
    nlast = now.tv_sec * 1000000000L + now.tv_nsec;
    arg = 0;
    do {
        if (url) {
            size_t len;
            char *jpg = readJpg(mjpeg, &len);
            if (!jpg) {
                barf("reading MJPEG stream");
                break;
            }
            write(vfd, jpg, len);
            fprintf(stderr, "\r%d  ", arg);
            fflush(stderr);
        } else {
            long nnow;
            int img = (arg/rate)%2;
            write(vfd, img ? bimg2 : bimg1, img ? simg2 : simg1);
            fprintf(stderr, "%d", img);
            fflush(stderr);
            clock_gettime(CLOCK_MONOTONIC, &now);
            nnow = now.tv_sec * 1000000000L + now.tv_nsec;
            now.tv_sec = 0;
            now.tv_nsec = nsecs - (nnow-nlast);
            nlast += nsecs;
            clock_nanosleep(CLOCK_MONOTONIC, 0, &now, NULL);
        }
        ++arg;
    } while(!done);
    fprintf(stderr, "Terminated.\n");
done:
    close(vfd);
    return 0;
}
