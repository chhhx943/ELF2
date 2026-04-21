#include "movies.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/mman.h>

#define VIDEO_DEV "/dev/video11"
#define WIDTH  1920
#define HEIGHT 1080
#define BUF_COUNT 4

struct Buffer
{
    void *start;
    size_t length;/* data */
};


int main()
{
    int fd;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_format fmt;
    struct v4l2_plane planes[1];
    struct Buffer buffers[BUF_COUNT];
    
    fd = open(VIDEO_DEV,O_RDWR);
    if(fd < 0)
    {
        perror("open()");
        exit(1);
    }
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = WIDTH;
    fmt.fmt.pix_mp.height = HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_UYVY;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes = 1;
    if(ioctl(fd,VIDIOC_S_FMT,&fmt)< 0)
    {
        perror("SetFormat");
        close(fd);
        exit(1);
    }

    memset(&req,0,sizeof(req));
    req.count = BUF_COUNT;
    req.type = fmt.type;
    req.memory = V4L2_MEMORY_MMAP;

    if(ioctl(fd,VIDIOC_REQBUFS,&req) < 0)
    {
        perror("request buffer()");
        close(fd);
        exit(1);
    }

    for(int i = 0;i < BUF_COUNT;++i)
    {
        memset(&buf,0,sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = fmt.type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;
        if(ioctl(fd,VIDIOC_QUERYBUF,&buf) < 0)
        {
            perror("query buffer()");
            close(fd);
            exit(1);
        }
        if (buf.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffers[i].length = buf.m.planes[0].length;
            buffers[i].start = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, 
                                MAP_SHARED, fd, buf.m.planes[0].m.mem_offset);
        } else {
            buffers[i].length = buf.length;
            buffers[i].start = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE, 
                                MAP_SHARED, fd, buf.m.offset);
        }
        if(buffers[i].start == MAP_FAILED)
        {
            perror("mmap()");
            close(fd);
            exit(1);

        }
        if(ioctl(fd,VIDIOC_QBUF,&buf) < 0)
        {
            perror("queue buf()");
            close(fd);
            exit(1);
        }
    }

    int type = buf.type;
    if(ioctl(fd,VIDIOC_STREAMON,&type) < 0)
    {
        perror("start stream()");
        close(fd);
        exit(1);
    }

 
    int outfd = open("stream.uyvy",O_WRONLY|O_CREAT|O_NONBLOCK,
                    0644);
    if(outfd < 0){
        perror("Failed to create output file");
        close(fd);
        exit(1);
    }
    int a = 0;
    while(1)
    {
        struct v4l2_buffer qbuf;
        struct v4l2_plane qplanes[1];
        memset(&qbuf,0,sizeof(qbuf));
        qbuf.type = fmt.type;
        qbuf.memory = V4L2_MEMORY_MMAP;
        qbuf.m.planes = qplanes;
        qbuf.length = 1;
        if(ioctl(fd,VIDIOC_DQBUF,&qbuf) < 0)
        {
            perror("dequeue buf()");
            break;
        }
        write(outfd, buffers[qbuf.index].start, 
                qbuf.m.planes[0].bytesused);
        
        if (ioctl(fd, VIDIOC_QBUF, &qbuf) < 0) {
            perror("QBUF");
            break;
        }
        a++;
        if(a > 100) break;
    }

    if(ioctl(fd,VIDIOC_STREAMOFF,&type) < 0){
        perror("stop stream()");
    }
    for (int i = 0; i < BUF_COUNT; i++) munmap(buffers[i].start, buffers[i].length);
    close(outfd);
    close(fd);
    exit(0);
}