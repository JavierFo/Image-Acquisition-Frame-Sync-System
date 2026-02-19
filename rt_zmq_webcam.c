#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <linux/videodev2.h>
#include <math.h>
#include <zmq.h> // ZeroMQ included

// --- Configuration ---
#define HRES 640
#define VRES 480
#define PIXEL_COUNT (HRES * VRES)
#define RT_CORE (2)
#define MY_CLOCK_TYPE CLOCK_MONOTONIC
#define SAVE_PATH "./frames"

#define TOTAL_RECORD_FRAMES (60) // 1 minute at 1Hz
#define HWM_LIMIT 10 // High Water Mark for ZMQ sockets

// IPC Endpoints
#define IPC_S1_S2 "ipc:///tmp/cam_s1_s2.ipc"
#define IPC_S2_S3 "ipc:///tmp/cam_s2_s3.ipc"
#define IPC_S3_S4 "ipc:///tmp/cam_s3_s4.ipc"

// --- Data Structures ---
typedef struct {
    unsigned int frame_id;
    double release_time;
    unsigned char data[PIXEL_COUNT];
} FrameMessage;

// --- Global State ---
void *zmq_ctx;
int abortTest = 0;
unsigned int frames_stored = 0;
double e2e_prev = 0.0;

// V4L2
struct buffer { void *start; size_t length; };
static int fd = -1;
struct buffer *buffers;
static unsigned int n_buffers;
unsigned char scratch_pad[PIXEL_COUNT];

// --- Function Prototypes ---
void *Service_1_Acquisition(void *threadp);
void *Service_2_Selection(void *threadp);
void *Service_3_Canny(void *threadp);
void *Service_4_Storage(void *threadp);
void wait_next_period(struct timespec *next_activation, long period_ns);

void CannyEdgeDetection(unsigned char *in_buffer, unsigned char *out_buffer, int width, int height);
double CalculateSharpness(unsigned char *image, int width, int height);
int v4l2_init(char *dev_name);
int v4l2_read_single(unsigned char *dest_buffer); 
int v4l2_shutdown(void);
double realtime(struct timespec *tsptr);

// --- Main ---
int main(int argc, char *argv[]) {
    char *dev_name = "/dev/video2";
    pthread_t threads[4];
    struct sched_param rt_param, main_param;
    pthread_attr_t rt_sched_attr;
    cpu_set_t threadcpu;

    openlog("rt_zmq_webcam", LOG_PID | LOG_CONS, LOG_USER);
    v4l2_init(dev_name);
    
    // Initialize ZMQ Context
    zmq_ctx = zmq_ctx_new();

    // Setup Scheduling
    int rt_max_prio = sched_get_priority_max(SCHED_FIFO);
    main_param.sched_priority = rt_max_prio;
    sched_setscheduler(getpid(), SCHED_FIFO, &main_param);

    pthread_attr_init(&rt_sched_attr);
    pthread_attr_setinheritsched(&rt_sched_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&rt_sched_attr, SCHED_FIFO);
    CPU_ZERO(&threadcpu);
    CPU_SET(RT_CORE, &threadcpu);
    pthread_attr_setaffinity_np(&rt_sched_attr, sizeof(cpu_set_t), &threadcpu);

    // Create Services (RM Priority Assignment)
    rt_param.sched_priority = rt_max_prio - 1; // S1 (30Hz)
    pthread_attr_setschedparam(&rt_sched_attr, &rt_param);
    pthread_create(&threads[0], &rt_sched_attr, Service_1_Acquisition, NULL);

    rt_param.sched_priority = rt_max_prio - 2; // S2 (10Hz)
    pthread_attr_setschedparam(&rt_sched_attr, &rt_param);
    pthread_create(&threads[1], &rt_sched_attr, Service_2_Selection, NULL);

    rt_param.sched_priority = rt_max_prio - 3; // S3 (5Hz)
    pthread_attr_setschedparam(&rt_sched_attr, &rt_param);
    pthread_create(&threads[2], &rt_sched_attr, Service_3_Canny, NULL);

    rt_param.sched_priority = rt_max_prio - 4; // S4 (1Hz)
    pthread_attr_setschedparam(&rt_sched_attr, &rt_param);
    pthread_create(&threads[3], &rt_sched_attr, Service_4_Storage, NULL);

    // Main thread waits for completion
    while(!abortTest) { sleep(1); }

    for(int i = 0; i < 4; i++) pthread_join(threads[i], NULL);

    v4l2_shutdown();
    zmq_ctx_destroy(zmq_ctx);
    closelog();
    printf("\nExecution Finished. Frames Stored: %d\n", frames_stored);
    return 0;
}

// --- Real-Time Wait Helper ---
void wait_next_period(struct timespec *next_activation, long period_ns) {
    next_activation->tv_nsec += period_ns;
    while (next_activation->tv_nsec >= 1000000000) {
        next_activation->tv_sec++;
        next_activation->tv_nsec -= 1000000000;
    }
    clock_nanosleep(MY_CLOCK_TYPE, TIMER_ABSTIME, next_activation, NULL);
}

// --- S1: Acquisition (30 Hz) ---
void *Service_1_Acquisition(void *threadp) {
    void *zmq_push = zmq_socket(zmq_ctx, ZMQ_PUSH);
    int hwm = HWM_LIMIT;
    zmq_setsockopt(zmq_push, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_bind(zmq_push, IPC_S1_S2);

    struct timespec next_activation;
    clock_gettime(MY_CLOCK_TYPE, &next_activation);
    unsigned int frame_cnt = 0;

    while(!abortTest) {
        wait_next_period(&next_activation, 33333333); // 33.3ms (30Hz)

        FrameMessage msg;
        struct timespec ts;
        clock_gettime(MY_CLOCK_TYPE, &ts);
        msg.release_time = realtime(&ts);
        msg.frame_id = frame_cnt++;

        if(v4l2_read_single(msg.data) == 0) {
            zmq_send(zmq_push, &msg, sizeof(FrameMessage), ZMQ_DONTWAIT);
        }
    }
    zmq_close(zmq_push);
    pthread_exit(NULL);
}

// --- S2: Frame Selection (10 Hz) ---
void *Service_2_Selection(void *threadp) {
    void *zmq_pull = zmq_socket(zmq_ctx, ZMQ_PULL);
    zmq_connect(zmq_pull, IPC_S1_S2);
    
    void *zmq_push = zmq_socket(zmq_ctx, ZMQ_PUSH);
    int hwm = HWM_LIMIT;
    zmq_setsockopt(zmq_push, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_bind(zmq_push, IPC_S2_S3);

    struct timespec next_activation;
    clock_gettime(MY_CLOCK_TYPE, &next_activation);

    while(!abortTest) {
        wait_next_period(&next_activation, 100000000); // 100ms (10Hz)

        FrameMessage incoming, best_frame;
        double best_sharpness = -1.0;
        int received_any = 0;

        // Drain queue: Evaluate all frames accumulated since last wake
        while (zmq_recv(zmq_pull, &incoming, sizeof(FrameMessage), ZMQ_DONTWAIT) > 0) {
            double sharpness = CalculateSharpness(incoming.data, HRES, VRES);
            if(sharpness > best_sharpness) {
                best_sharpness = sharpness;
                memcpy(&best_frame, &incoming, sizeof(FrameMessage));
            }
            received_any = 1;
        }

        // Push only the sharpest frame forward
        if(received_any) {
            zmq_send(zmq_push, &best_frame, sizeof(FrameMessage), ZMQ_DONTWAIT);
        }
    }
    zmq_close(zmq_pull);
    zmq_close(zmq_push);
    pthread_exit(NULL);
}

// --- S3: Canny Edge (5 Hz) ---
void *Service_3_Canny(void *threadp) {
    void *zmq_pull = zmq_socket(zmq_ctx, ZMQ_PULL);
    zmq_connect(zmq_pull, IPC_S2_S3);
    
    void *zmq_push = zmq_socket(zmq_ctx, ZMQ_PUSH);
    int hwm = HWM_LIMIT;
    zmq_setsockopt(zmq_push, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_bind(zmq_push, IPC_S3_S4);

    struct timespec next_activation;
    clock_gettime(MY_CLOCK_TYPE, &next_activation);

    while(!abortTest) {
        wait_next_period(&next_activation, 200000000); // 200ms (5Hz)

        FrameMessage incoming, fresh_frame;
        int received_any = 0;

        // Drain queue: Keep only the freshest frame (drop older ones)
        while (zmq_recv(zmq_pull, &incoming, sizeof(FrameMessage), ZMQ_DONTWAIT) > 0) {
            memcpy(&fresh_frame, &incoming, sizeof(FrameMessage));
            received_any = 1;
        }

        if(received_any) {
            FrameMessage out_msg;
            out_msg.frame_id = fresh_frame.frame_id;
            out_msg.release_time = fresh_frame.release_time; // Carry S1's timestamp
            CannyEdgeDetection(fresh_frame.data, out_msg.data, HRES, VRES);
            zmq_send(zmq_push, &out_msg, sizeof(FrameMessage), ZMQ_DONTWAIT);
        }
    }
    zmq_close(zmq_pull);
    zmq_close(zmq_push);
    pthread_exit(NULL);
}

// --- S4: Storage & Telemetry (1 Hz) ---
void *Service_4_Storage(void *threadp) {
    void *zmq_pull = zmq_socket(zmq_ctx, ZMQ_PULL);
    zmq_connect(zmq_pull, IPC_S3_S4);

    struct timespec next_activation;
    clock_gettime(MY_CLOCK_TYPE, &next_activation);

    while(!abortTest && frames_stored < TOTAL_RECORD_FRAMES) {
        wait_next_period(&next_activation, 1000000000); // 1s (1Hz)

        FrameMessage incoming, fresh_frame;
        int received_any = 0;

        // Drain queue for freshest frame
        while (zmq_recv(zmq_pull, &incoming, sizeof(FrameMessage), ZMQ_DONTWAIT) > 0) {
            memcpy(&fresh_frame, &incoming, sizeof(FrameMessage));
            received_any = 1;
        }

        if(received_any) {
            char filename[256];
            snprintf(filename, sizeof(filename), "%s/frame_%04u.pgm", SAVE_PATH, frames_stored + 1);
            int out_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(out_fd >= 0) {
                dprintf(out_fd, "P5\n%d %d\n255\n", HRES, VRES);
                write(out_fd, fresh_frame.data, PIXEL_COUNT);
                close(out_fd);
            }

            // Metrics calculation
            struct timespec ts_end;
            clock_gettime(MY_CLOCK_TYPE, &ts_end);
            
            double e2e_latency_ms = (realtime(&ts_end) - fresh_frame.release_time) * 1000.0;
            double jitter_ms = (frames_stored == 0) ? 0.0 : (e2e_latency_ms - e2e_prev);
            e2e_prev = e2e_latency_ms;

            syslog(LOG_CRIT, "[RT_ZMQ] Frame:%u Cycle_Latency_ms=%.3f Jitter_ms=%.3f", 
                   frames_stored + 1, e2e_latency_ms, jitter_ms);

            frames_stored++;
            printf("Stored ZMQ Frame: %d/%d (Latency: %.2fms)\n", frames_stored, TOTAL_RECORD_FRAMES, e2e_latency_ms);
        }
    }
    abortTest = 1; // Signal all threads to exit
    zmq_close(zmq_pull);
    pthread_exit(NULL);
}

// --- Image Processing Utilities ---
double CalculateSharpness(unsigned char *image, int width, int height) {
    long sum = 0, sum_sq = 0; int count = 0;
    for(int i = 1; i < height - 1; i += 2) {
        for(int j = 1; j < width - 1; j += 2) {
            int lap = 4 * image[i*width + j] - image[(i-1)*width + j] - image[(i+1)*width + j] - image[i*width + (j-1)] - image[i*width + (j+1)];
            sum += lap; sum_sq += lap * lap; count++;
        }
    }
    double mean = (double)sum / count;
    return ((double)sum_sq / count) - (mean * mean);
}

void CannyEdgeDetection(unsigned char *in_buffer, unsigned char *out_buffer, int width, int height) {
    int i, j, gx, gy, sum;
    for(i = 1; i < height - 1; i++) {
        for(j = 1; j < width - 1; j++) {
            sum = 0;
            sum += in_buffer[(i-1)*width + (j-1)] + in_buffer[(i-1)*width + j] + in_buffer[(i-1)*width + (j+1)];
            sum += in_buffer[i*width + (j-1)]     + in_buffer[i*width + j]     + in_buffer[i*width + (j+1)];
            sum += in_buffer[(i+1)*width + (j-1)] + in_buffer[(i+1)*width + j] + in_buffer[(i+1)*width + (j+1)];
            scratch_pad[i*width + j] = sum / 9;
        }
    }
    memset(out_buffer, 0, width*height); 
    for(i = 1; i < height - 1; i++) {
        for(j = 1; j < width - 1; j++) {
            gx = -1*scratch_pad[(i-1)*width + (j-1)] + 1*scratch_pad[(i-1)*width + (j+1)]
                 -2*scratch_pad[i*width + (j-1)]     + 2*scratch_pad[i*width + (j+1)]
                 -1*scratch_pad[(i+1)*width + (j-1)] + 1*scratch_pad[(i+1)*width + (j+1)];

            gy = -1*scratch_pad[(i-1)*width + (j-1)] - 2*scratch_pad[(i-1)*width + j] - 1*scratch_pad[(i-1)*width + (j+1)]
                 +1*scratch_pad[(i+1)*width + (j-1)] + 2*scratch_pad[(i+1)*width + j] + 1*scratch_pad[(i+1)*width + (j+1)];

            int mag = abs(gx) + abs(gy);
            if(mag > 90) out_buffer[i*width + j] = 255;
            else if (mag > 30) out_buffer[i*width + j] = 100;
        }
    }
}

// --- V4L2 Read & Init ---
int v4l2_read_single(unsigned char *dest_buffer) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP;
    if(ioctl(fd, VIDIOC_DQBUF, &buf) < 0) return -1;
    unsigned char *src = (unsigned char *)buffers[buf.index].start;
    for(int i = 0; i < PIXEL_COUNT; i++) dest_buffer[i] = src[i*2]; // Y channel
    ioctl(fd, VIDIOC_QBUF, &buf);
    return 0;
}

int v4l2_init(char *dev_name) {
    struct v4l2_format fmt = {0}; struct v4l2_requestbuffers req = {0};
    fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; fmt.fmt.pix.width = HRES; fmt.fmt.pix.height = VRES;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; ioctl(fd, VIDIOC_S_FMT, &fmt);
    req.count = 10; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &req);
    buffers = calloc(req.count, sizeof(*buffers));
    for(n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        struct v4l2_buffer buf = {0}; buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP; buf.index = n_buffers;
        ioctl(fd, VIDIOC_QUERYBUF, &buf); buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = mmap(NULL, buf.length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        ioctl(fd, VIDIOC_QBUF, &buf);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(fd, VIDIOC_STREAMON, &type);
    return 0;
}

int v4l2_shutdown(void) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(fd, VIDIOC_STREAMOFF, &type);
    for(int i = 0; i < n_buffers; ++i) munmap(buffers[i].start, buffers[i].length);
    close(fd); return 0;
}

double realtime(struct timespec *tsptr) {
    return ((double)(tsptr->tv_sec) + (((double)tsptr->tv_nsec)/1000000000.0));
}
