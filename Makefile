CROSS_COMPILE = /home/c/linux/ELF2/work/ELF2-linux-source/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

CC = $(CROSS_COMPILE)gcc

# 1. 【编译时】头文件目录：用你原? v62 的头文件，完全没问题
FFMPEG_INC =/home/c/ffmpeg-6.0
RGA_INC = /home/c/linux/ELF2/work/ELF2-linux-source/Project/camera_flow/librga/include
ELF2_EXT_SYSROOT = $(CURDIR)/third_party/elf2_sysroot
ELF2_EXT_INC = $(ELF2_EXT_SYSROOT)/usr/include
ELF2_EXT_ARCH_INC = $(ELF2_EXT_SYSROOT)/usr/include/aarch64-linux-gnu
ELF2_EXT_LIB = $(ELF2_EXT_SYSROOT)/usr/lib/aarch64-linux-gnu
# 2. 【链接时】库文件目录：指向你从板子拉回来的克隆目?
SYSROOT_LIB = /home/c/rk3588_sysroot/usr/lib/

# 加入头文件搜索路? (如果你之前编? MPP 的头文件在别处，请在这里? -I 加上)
CFLAGS = -Wall -O2 -g -I$(FFMPEG_INC) -I$(RGA_INC) -I$(ELF2_EXT_INC) -I$(ELF2_EXT_ARCH_INC) -Wl,--allow-shlib-undefined

# 3. 【核心链接指令】：严格绑定 .60 版本和瑞芯微系统路径
LDFLAGS = -L$(SYSROOT_LIB) \
          -L$(ELF2_EXT_LIB) \
          -Wl,-rpath=/usr/lib/aarch64-linux-gnu \
          -l:libavformat.so.60 \
          -l:libavcodec.so.60 \
          -l:libswscale.so.7 \
          -l:libswresample.so.4 \
          -l:libavutil.so.58 \
          -lpaho-mqtt3c \
          -lmodbus \
          -lcurl \
          -lsqlite3 \
          -ldrm \
          -lrga \
          -lm -lpthread

TARGET = main
STORE_DEBUG_TARGET = store_debug
MQTT_CHAIN_DEBUG_TARGET = mqtt_chain_debug
VIDEO_UPLOADER_DEBUG_TARGET = video_uploader_debug
SRCS = main.c camera.c ipc.c encoder.c aliyun_mqtt.c sensor_modbus.c local_store.c video_store.c video_uploader.c
OBJS = $(SRCS:.c=.o)
STORE_DEBUG_OBJS = store_debug.o local_store.o
MQTT_CHAIN_DEBUG_OBJS = mqtt_chain_debug.o aliyun_mqtt.o local_store.o
VIDEO_UPLOADER_DEBUG_OBJS = video_uploader_debug.o local_store.o video_store.o video_uploader.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(STORE_DEBUG_TARGET): $(STORE_DEBUG_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -L$(ELF2_EXT_LIB) -Wl,-rpath=/usr/lib/aarch64-linux-gnu -lsqlite3

$(MQTT_CHAIN_DEBUG_TARGET): $(MQTT_CHAIN_DEBUG_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -L$(ELF2_EXT_LIB) -Wl,-rpath=/usr/lib/aarch64-linux-gnu -lpaho-mqtt3c -lsqlite3 -lm -lpthread

$(VIDEO_UPLOADER_DEBUG_TARGET): $(VIDEO_UPLOADER_DEBUG_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -L$(ELF2_EXT_LIB) -Wl,-rpath=/usr/lib/aarch64-linux-gnu -lcurl -lsqlite3 -lm -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(STORE_DEBUG_OBJS) $(MQTT_CHAIN_DEBUG_OBJS) $(VIDEO_UPLOADER_DEBUG_OBJS) $(TARGET) $(STORE_DEBUG_TARGET) $(MQTT_CHAIN_DEBUG_TARGET) $(VIDEO_UPLOADER_DEBUG_TARGET)

.PHONY: all clean $(STORE_DEBUG_TARGET) $(MQTT_CHAIN_DEBUG_TARGET) $(VIDEO_UPLOADER_DEBUG_TARGET)
