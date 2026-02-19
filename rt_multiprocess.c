#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
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
#include <sys/wait.h>
#include <linux/videodev2.h>
#include <math.h>

// --- Configuration ---
#define HRES 640
#define VRES 480
#define PIXEL_COUNT (HRES * VRES)
#define RT_CORE (2)
#define MY_CLOCK_TYPE CLOCK_MONOTONIC_RAW
#define SAVE_PATH "./frames" // Ensure this directory exists

#define WARMUP_LIMIT (30)
#define TOTAL_RECORD_FRAMES (300)
#define RING_SIZE (1024) 

// --- Data Structures ---
typedef struct {
    double release_time;
    unsigned char data[PIXEL_COUNT];
} FrameData;

// Shared Memory Structure for IPC
typedef struct {
    sem_t semS1, semS2, semS3, semS4;
    int abortTest;
    unsigned int frames_acquired; 
    unsigned int frames_selected;
    unsigned int frames_processed;
    unsigned int frames_stored;
    
    FrameData s1_buffer[3];          // S1 writes, S2 reads
    FrameData s2_buffer[RING_SIZE];  // S2 writes, S3 reads
    FrameData s3_buffer[RING_SIZE];  // S3 writes, S4 reads
} SharedData;

SharedData *shm; // Pointer to shared memory mapping

// Local to each process (not shared)
unsigned char scratch_pad[PIXEL_COUNT];
double e2e_prev = 0.0; // Local to S4

// V4L2
struct buffer { void *start; size_t length; };
static int fd = -1;
struct buffer *buffers;
static unsigned int n_buffers;

// --- Function Prototypes ---
void Sequencer(int id);
void Service_1_Acquisition(void);
void Service_2_Selection(void);
void Service_3_Canny(void);
void Service_4_Storage(void);

void CannyEdgeDetection(unsigned char *in_buffer, unsigned char *out_buffer, int width, int height);
double CalculateSharpness(unsigned char *image, int width, int height);
int v4l2_init(char *dev_name);
int v4l2_read_single(unsigned char *dest_buffer); 
int v4l2_shutdown(void);
double realtime(struct timespec *tsptr);
void set_process_rt_priority(int policy, int priority, int core);

// --- Main ---
int main(int argc, char *argv[]) {
    char *dev_name = "/dev/video0";
    pid_t pids[4];
    int i;

    // 1. Allocate Shared Memory
    shm = mmap(NULL, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }

    memset(shm, 0, sizeof(SharedData));

    // 2. Initialize Shared Semaphores (pshared = 1)
    sem_init(&shm->semS1, 1, 0); 
    sem_init(&shm->semS2, 1, 0); 
    sem_init(&shm->semS3, 1, 0); 
    sem_init(&shm->semS4, 1, 0);

    openlog("rt_webcam", LOG_PID | LOG_CONS, LOG_USER);
    
    // Initialize V4L2 in parent so children inherit the open FD and memory maps
    if (v4l2_init(dev_name) != 0) {
        printf("Failed to initialize V4L2.\n");
        exit(EXIT_FAILURE);
    }

    int rt_max_prio = sched_get_priority_max(SCHED_FIFO);

    // 3. Fork Processes
    // S1: 30 Hz (Highest Prio)
    if ((pids[0] = fork()) == 0) {
        set_process_rt_priority(SCHED_FIFO, rt_max_prio - 1, RT_CORE);
        Service_1_Acquisition();
        exit(0);
    }

    // S2: 10 Hz
    if ((pids[1] = fork()) == 0) {
        set_process_rt_priority(SCHED_FIFO, rt_max_prio - 2, RT_CORE);
        Service_2_Selection();
        exit(0);
    }

    // S3: 5 Hz
    if ((pids[2] = fork()) == 0) {
        set_process_rt_priority(SCHED_FIFO, rt_max_prio - 3, RT_CORE);
        Service_3_Canny();
        exit(0);
    }

    // S4: 1 Hz (Lowest Prio)
    if ((pids[3] = fork()) == 0) {
        set_process_rt_priority(SCHED_FIFO, rt_max_prio - 4, RT_CORE);
        Service_4_Storage();
        exit(0);
    }

    // 4. Setup Parent as Sequencer
    set_process_rt_priority(SCHED_FIFO, rt_max_prio, RT_CORE);

    struct itimerspec itime = {{0, 3333333}, {0, 3333333}};
    timer_t timer_1;
    timer_create(CLOCK_REALTIME, NULL, &timer_1);
    signal(SIGALRM, Sequencer);
    timer_settime(timer_1, 0, &itime, NULL);

    // Wait for all children to exit
    for(i = 0; i < 4; i++) {
        waitpid(pids[i], NULL, 0);
    }

    // 5. Cleanup
    v4l2_shutdown();
    closelog();
    
    printf("\nExecution Finished. Frames Stored: %u\n", shm->frames_stored);
    
    munmap(shm, sizeof(SharedData));
    return 0;
}

// --- RT Priority & Affinity Helper ---
void set_process_rt_priority(int policy, int priority, int core) {
    struct sched_param param;
    param.sched_priority = priority;
    
    if (sched_setscheduler(0, policy, &param) == -1) {
        perror("sched_setscheduler failed (run with sudo!)");
    }

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
        perror("sched_setaffinity failed");
    }
}

// --- Sequencer (300 Hz Base) ---
void Sequencer(int id) {
    static unsigned long long seqCnt = 0;
    seqCnt++;

    if(shm->abortTest) return;

    // S1 @ 30 Hz (Every 10 ticks)
    if((seqCnt % 10) == 0) sem_post(&shm->semS1);

    // Wait for warmup before triggering downstream
    if(shm->frames_acquired > WARMUP_LIMIT) {
        if((seqCnt % 30) == 0) sem_post(&shm->semS2);
        if((seqCnt % 60) == 0) sem_post(&shm->semS3);
        if((seqCnt % 300) == 0) sem_post(&shm->semS4);
    }

    if(shm->frames_stored >= TOTAL_RECORD_FRAMES) {
        shm->abortTest = 1;
        sem_post(&shm->semS1); sem_post(&shm->semS2); 
        sem_post(&shm->semS3); sem_post(&shm->semS4);
    }
}

// --- S1: Acquisition (30 Hz) ---
void Service_1_Acquisition(void) {
    while(!shm->abortTest) {
        sem_wait(&shm->semS1);
        if(shm->abortTest) break;

        struct timespec ts;
        clock_gettime(MY_CLOCK_TYPE, &ts);
        
        int idx = shm->frames_acquired % 3;
        shm->s1_buffer[idx].release_time = realtime(&ts);

        if(v4l2_read_single(shm->s1_buffer[idx].data) == 0) {
            shm->frames_acquired++;
        }
    }
}

// --- S2: Frame Selection (10 Hz) ---
void Service_2_Selection(void) {
    while(!shm->abortTest) {
        sem_wait(&shm->semS2);
        if(shm->abortTest) break;

        double best_sharpness = -1.0;
        int best_idx = 0;

        for(int i = 0; i < 3; i++) {
            double sharpness = CalculateSharpness(shm->s1_buffer[i].data, HRES, VRES);
            if(sharpness > best_sharpness) {
                best_sharpness = sharpness;
                best_idx = i;
            }
        }

        int s2_idx = shm->frames_selected % RING_SIZE;
        memcpy(&shm->s2_buffer[s2_idx], &shm->s1_buffer[best_idx], sizeof(FrameData));
        shm->frames_selected++;
    }
}

// --- S3: Canny Edge (5 Hz) ---
void Service_3_Canny(void) {
    while(!shm->abortTest) {
        sem_wait(&shm->semS3);
        if(shm->abortTest) break;

        if(shm->frames_selected > 0) {
            int src_idx = (shm->frames_selected - 1) % RING_SIZE;
            int dst_idx = shm->frames_processed % RING_SIZE;

            shm->s3_buffer[dst_idx].release_time = shm->s2_buffer[src_idx].release_time;
            CannyEdgeDetection(shm->s2_buffer[src_idx].data, shm->s3_buffer[dst_idx].data, HRES, VRES);
            shm->frames_processed++;
        }
    }
}

// --- S4: Storage & Telemetry (1 Hz) ---
void Service_4_Storage(void) {
    while(!shm->abortTest) {
        sem_wait(&shm->semS4);
        if(shm->abortTest) break;

        if(shm->frames_processed > 0) {
            int idx = (shm->frames_processed - 1) % RING_SIZE;
            
            // Save to disk
            char filename[256];
            snprintf(filename, sizeof(filename), "%s/frame_%04u.pgm", SAVE_PATH, shm->frames_stored + 1);
            int out_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if(out_fd >= 0) {
                dprintf(out_fd, "P5\n%d %d\n255\n", HRES, VRES);
                write(out_fd, shm->s3_buffer[idx].data, PIXEL_COUNT);
                close(out_fd);
            }

            // End-to-End Latency & Jitter Calculation
            struct timespec ts_end;
            clock_gettime(MY_CLOCK_TYPE, &ts_end);
            
            double e2e_latency_ms = (realtime(&ts_end) - shm->s3_buffer[idx].release_time) * 1000.0;
            double jitter_ms = (shm->frames_stored == 0) ? 0.0 : (e2e_latency_ms - e2e_prev);
            e2e_prev = e2e_latency_ms;

            syslog(LOG_CRIT, "[RT_MULTIPROCESS] Frame:%u Cycle_Latency_ms=%.3f Jitter_ms=%.3f", 
                   shm->frames_stored + 1, e2e_latency_ms, jitter_ms);

            shm->frames_stored++;
            printf("Stored: %d/%d (Latency: %.2f ms | Jitter: %.2f ms)\n", 
                    shm->frames_stored, TOTAL_RECORD_FRAMES, e2e_latency_ms, jitter_ms);
        }
    }
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

// --- Standard Canny ---
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
    for(int i = 0; i < n_buffers; ++i) munmap(buffers[i].start, buffers[i].length);
    close(fd);
    free(buffers);
    return 0;
}

double realtime(struct timespec *tsptr) {
    return ((double)(tsptr->tv_sec) + (((double)tsptr->tv_nsec)/1000000000.0));
}
