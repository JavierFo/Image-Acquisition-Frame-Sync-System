#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <semaphore.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <linux/videodev2.h>
#include <math.h>

// --- Configuration ---
#define HRES 640
#define VRES 480
#define PIXEL_COUNT (HRES * VRES)
#define MY_CLOCK_TYPE CLOCK_MONOTONIC_RAW
#define SAVE_PATH "./frames" // Ensure this directory exists!

#define WARMUP_LIMIT (30)
#define TOTAL_RECORD_FRAMES (300)
#define RING_SIZE (1024) 

// --- Data Structures ---
typedef struct {
    double release_time;
    unsigned char data[PIXEL_COUNT];
} FrameData;

// --- Global State ---
sem_t semS1, semS2, semS3, semS4;
timer_t timer_1;
int abortTest = 0;

// Counters
unsigned int frames_acquired = 0; 
unsigned int frames_selected = 0;
unsigned int frames_processed = 0;
unsigned int frames_stored = 0;

// Pipeline Buffers
FrameData s1_buffer[3];          
FrameData s2_buffer[RING_SIZE];  
FrameData s3_buffer[RING_SIZE];  
unsigned char scratch_pad[PIXEL_COUNT];

// V4L2
struct buffer { void *start; size_t length; };
static int fd = -1;
struct buffer *buffers;
static unsigned int n_buffers;

// Metrics
double e2e_prev = 0.0;

// --- Function Prototypes ---
void Sequencer(int id);
void *Service_1_Acquisition(void *threadp);
void *Service_2_Selection(void *threadp);
void *Service_3_Canny(void *threadp);
void *Service_4_Storage(void *threadp);

void CannyEdgeDetection(unsigned char *in_buffer, unsigned char *out_buffer, int width, int height);
double CalculateSharpness(unsigned char *image, int width, int height);
int v4l2_init(char *dev_name);
int v4l2_read_single(unsigned char *dest_buffer); 
int v4l2_shutdown(void);
double realtime(struct timespec *tsptr);
void pin_thread_to_core(pthread_t thread, int core_id);

// --- Main ---
int main(int argc, char *argv[]) {
    char *dev_name = "/dev/video0";
    pthread_t threads[4];
    struct sched_param rt_param, main_param;
    pthread_attr_t rt_sched_attr;

    openlog("rt_MULTICORE", LOG_PID | LOG_CONS, LOG_USER);
    //syslog(LOG_INFO, "Starting Multi-Core RT Webcam Pipeline");
    
    if (v4l2_init(dev_name) != 0) {
        printf("Failed to init V4L2. Ensure %s is connected.\n", dev_name);
        return -1;
    }

    sem_init(&semS1, 0, 0); sem_init(&semS2, 0, 0); 
    sem_init(&semS3, 0, 0); sem_init(&semS4, 0, 0);

    // Main Thread Priority
    int rt_max_prio = sched_get_priority_max(SCHED_FIFO);
    main_param.sched_priority = rt_max_prio;
    sched_setscheduler(getpid(), SCHED_FIFO, &main_param);

    // Thread Attributes (Generic RT initialization)
    pthread_attr_init(&rt_sched_attr);
    pthread_attr_setinheritsched(&rt_sched_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&rt_sched_attr, SCHED_FIFO);

    // S1: 30 Hz (Highest Prio) - Pin to Core 1
    rt_param.sched_priority = rt_max_prio - 1;
    pthread_attr_setschedparam(&rt_sched_attr, &rt_param);
    pthread_create(&threads[0], &rt_sched_attr, Service_1_Acquisition, NULL);
    pin_thread_to_core(threads[0], 1); 

    // S2: 10 Hz - Pin to Core 2
    rt_param.sched_priority = rt_max_prio - 2;
    pthread_attr_setschedparam(&rt_sched_attr, &rt_param);
    pthread_create(&threads[1], &rt_sched_attr, Service_2_Selection, NULL);
    pin_thread_to_core(threads[1], 2);

    // S3: 5 Hz - Pin to Core 3
    rt_param.sched_priority = rt_max_prio - 3;
    pthread_attr_setschedparam(&rt_sched_attr, &rt_param);
    pthread_create(&threads[2], &rt_sched_attr, Service_3_Canny, NULL);
    pin_thread_to_core(threads[2], 3);

    // S4: 1 Hz (Lowest Prio) - Pin to Core 4
    rt_param.sched_priority = rt_max_prio - 4;
    pthread_attr_setschedparam(&rt_sched_attr, &rt_param);
    pthread_create(&threads[3], &rt_sched_attr, Service_4_Storage, NULL);
    pin_thread_to_core(threads[3], 4); // Assuming at least a quad-core system

    // Start 300Hz Sequencer (3.333 ms)
    struct itimerspec itime = {{0, 3333333}, {0, 3333333}};
    timer_create(CLOCK_REALTIME, NULL, &timer_1);
    signal(SIGALRM, (void(*)()) Sequencer);
    timer_settime(timer_1, 0, &itime, NULL);

    for(int i = 0; i < 4; i++) pthread_join(threads[i], NULL);

    v4l2_shutdown();
    closelog();
    printf("\nExecution Finished. Frames Stored: %d\n", frames_stored);
    return 0;
}

// Helper to assign specific threads to specific cores
void pin_thread_to_core(pthread_t thread, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
        syslog(LOG_WARNING, "Failed to pin thread to core %d. Proceeding unpinned.", core_id);
    }
}

// --- Sequencer (300 Hz Base) ---
void Sequencer(int id) {
    static unsigned long long seqCnt = 0;
    seqCnt++;

    if(abortTest) return;

    if((seqCnt % 10) == 0) sem_post(&semS1); // S1 @ 30 Hz

    if(frames_acquired > WARMUP_LIMIT) {
        if((seqCnt % 30) == 0) sem_post(&semS2);  // S2 @ 10 Hz
        if((seqCnt % 60) == 0) sem_post(&semS3);  // S3 @ 5 Hz
        if((seqCnt % 300) == 0) sem_post(&semS4); // S4 @ 1 Hz
    }

    if(frames_stored >= TOTAL_RECORD_FRAMES) {
        abortTest = 1;
        sem_post(&semS1); sem_post(&semS2); 
        sem_post(&semS3); sem_post(&semS4);
    }
}

// --- S1: Acquisition (30 Hz) ---
void *Service_1_Acquisition(void *threadp) {
    while(!abortTest) {
        sem_wait(&semS1);
        if(abortTest) break;

        struct timespec ts;
        clock_gettime(MY_CLOCK_TYPE, &ts);
        
        int idx = frames_acquired % 3;
        s1_buffer[idx].release_time = realtime(&ts);

        if(v4l2_read_single(s1_buffer[idx].data) == 0) {
            frames_acquired++;
        }
    }
    pthread_exit(NULL);
}

// --- S2: Frame Selection (10 Hz) ---
void *Service_2_Selection(void *threadp) {
    while(!abortTest) {
        sem_wait(&semS2);
        if(abortTest) break;

        double best_sharpness = -1.0;
        int best_idx = 0;

        for(int i = 0; i < 3; i++) {
            double sharpness = CalculateSharpness(s1_buffer[i].data, HRES, VRES);
            if(sharpness > best_sharpness) {
                best_sharpness = sharpness;
                best_idx = i;
            }
        }

        int s2_idx = frames_selected % RING_SIZE;
        memcpy(&s2_buffer[s2_idx], &s1_buffer[best_idx], sizeof(FrameData));
        frames_selected++;
    }
    pthread_exit(NULL);
}

// --- S3: Canny Edge (5 Hz) ---
void *Service_3_Canny(void *threadp) {
    while(!abortTest) {
        sem_wait(&semS3);
        if(abortTest) break;

        if(frames_selected > 0) {
            int src_idx = (frames_selected - 1) % RING_SIZE;
            int dst_idx = frames_processed % RING_SIZE;

            s3_buffer[dst_idx].release_time = s2_buffer[src_idx].release_time; 
            CannyEdgeDetection(s2_buffer[src_idx].data, s3_buffer[dst_idx].data, HRES, VRES);
            frames_processed++;
        }
    }
    pthread_exit(NULL);
}

// --- S4: Storage & Telemetry (1 Hz) ---
void *Service_4_Storage(void *threadp) {
    while(!abortTest) {
        sem_wait(&semS4);
        if(abortTest) break;

        if(frames_processed > 0) {
            int idx = (frames_processed - 1) % RING_SIZE;
            
            char filename[256];
            snprintf(filename, sizeof(filename), "%s/frame_%04u.pgm", SAVE_PATH, frames_stored + 1);
            int out_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(out_fd >= 0) {
                dprintf(out_fd, "P5\n%d %d\n255\n", HRES, VRES);
                write(out_fd, s3_buffer[idx].data, PIXEL_COUNT);
                close(out_fd);
            }

            // End-to-End Latency & Jitter Calculation
            struct timespec ts_end;
            clock_gettime(MY_CLOCK_TYPE, &ts_end);
            
            double e2e_latency_ms = (realtime(&ts_end) - s3_buffer[idx].release_time) * 1000.0;
            double jitter_ms = (frames_stored == 0) ? 0.0 : (e2e_latency_ms - e2e_prev);
            e2e_prev = e2e_latency_ms;

            // Log securely to syslog
            syslog(LOG_CRIT, "[RT_MULTICORE] Frame:%u Cycle_Latency_ms=%.3f Jitter_ms=%.3f", 
                   frames_stored + 1, e2e_latency_ms, jitter_ms);

            frames_stored++;
            printf("Stored: %d/%d (Latency: %.3fms)\n", frames_stored, TOTAL_RECORD_FRAMES, e2e_latency_ms);
        }
    }
    pthread_exit(NULL);
}

// --- Computer Vision Utility: Sharpness (Variance of Laplacian) ---
double CalculateSharpness(unsigned char *image, int width, int height) {
    long sum = 0, sum_sq = 0;
    int count = 0;
    
    for(int i = 1; i < height - 1; i += 2) {
        for(int j = 1; j < width - 1; j += 2) {
            int lap = 4 * image[i*width + j] 
                      - image[(i-1)*width + j] - image[(i+1)*width + j] 
                      - image[i*width + (j-1)] - image[i*width + (j+1)];
            sum += lap;
            sum_sq += lap * lap;
            count++;
        }
    }
    double mean = (double)sum / count;
    return ((double)sum_sq / count) - (mean * mean);
}

// --- Standard Canny (Unchanged) ---
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

// --- V4L2 Read Single ---
int v4l2_read_single(unsigned char *dest_buffer) {
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    
    if(ioctl(fd, VIDIOC_DQBUF, &buf) < 0) return -1;

    unsigned char *src = (unsigned char *)buffers[buf.index].start;
    for(int i = 0; i < PIXEL_COUNT; i++) dest_buffer[i] = src[i*2]; 
    
    ioctl(fd, VIDIOC_QBUF, &buf);
    return 0;
}

// --- V4L2 Init / Shutdown ---
int v4l2_init(char *dev_name) {
    struct v4l2_format fmt = {0};
    struct v4l2_requestbuffers req = {0};

    fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);
    if(fd < 0) return -1;
    
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = HRES; 
    fmt.fmt.pix.height = VRES;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    ioctl(fd, VIDIOC_S_FMT, &fmt);

    req.count = 10; 
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &req);

    buffers = calloc(req.count, sizeof(*buffers));
    for(n_buffers = 0; n_buffers < req.count; ++n_buffers) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;
        ioctl(fd, VIDIOC_QUERYBUF, &buf);
        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = mmap(NULL, buf.length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        ioctl(fd, VIDIOC_QBUF, &buf);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMON, &type);
    return 0;
}

int v4l2_shutdown(void) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for(unsigned int i = 0; i < n_buffers; ++i) munmap(buffers[i].start, buffers[i].length);
    close(fd);
    return 0;
}

double realtime(struct timespec *tsptr) {
    return ((double)(tsptr->tv_sec) + (((double)tsptr->tv_nsec)/1000000000.0));
}
