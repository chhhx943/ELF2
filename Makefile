CROSS_COMPILE = /home/c/linux/ELF2/work/ELF2-linux-source/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

CC = $(CROSS_COMPILE)gcc

# 1. 【编译时】头文件目录：用你原来 v62 的头文件，完全没问题
FFMPEG_INC =/home/c/ffmpeg-6.0

# 2. 【链接时】库文件目录：指向你从板子拉回来的克隆目录
SYSROOT_LIB = /home/c/rk3588_sysroot/usr/lib/

# 加入头文件搜索路径 (如果你之前编译 MPP 的头文件在别处，请在这里用 -I 加上)
CFLAGS = -Wall -O2 -g -I$(FFMPEG_INC) -Wl,--allow-shlib-undefined

# 3. 【核心链接指令】：严格绑定 .60 版本和瑞芯微系统路径
LDFLAGS = -L$(SYSROOT_LIB) \
          -Wl,-rpath=/usr/lib/aarch64-linux-gnu \
          -l:libavformat.so.60 \
          -l:libavcodec.so.60 \
          -l:libswscale.so.7 \
          -l:libswresample.so.4 \
          -l:libavutil.so.58 \
          -lm -lpthread

TARGET = movies
SRCS = movies.c encoder.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean