#define _GNU_SOURCE

#include "bktpool.h"

#include <stdio.h>

#include <sched.h>
//#include <linux/sched.h>
#include <sys/syscall.h>      /* Definition of SYS_* constants */
#include <unistd.h>

//#define DEBUG
#define INFO
#define WORK_THREAD

int bkwrk_worker(void * arg) {
    sigset_t set;
    int sig;
    int s;
    int i = * ((int * ) arg); // Default arg is integer of workid
    struct bkworker_t * wrk = & worker[i];

    /* Taking the mask for waking up */
    sigemptyset(&set);
    sigaddset(&set, SIG_DISPATCH);
    sigaddset(&set, SIGQUIT);

#ifdef DEBUG
    fprintf(stderr, "worker %i start living tid %d \n", i, getpid());
    fflush(stderr);
#endif

    while (1) {
        /* wait for signal */
        s = sigwait(&set, &sig);
        if (s != 0)
            continue;

#ifdef INFO
        fprintf(stderr, "worker wake %d up\n", i);
#endif

        if (sig == SIG_DISPATCH) {
            /* Busy running */
            if (wrk->func != NULL)
                wrk->func(wrk->arg);

            /* Advertise I DONE WORKING */
            wrkid_busy[i] = 0;
            worker[i].func = NULL;
            worker[i].arg = NULL;
            worker[i].bktaskid = -1;
        } else if (sig == SIGQUIT) {
            break;  // Exit loop on SIGQUIT
        }
    }
    return 0;
}




int bktask_assign_worker(unsigned int bktaskid, unsigned int wrkid) {
    // Kiểm tra xem ID của worker có hợp lệ hay không
    if (wrkid < 0 || wrkid >= MAX_WORKER) {
        printf("Invalid worker ID: %u\n", wrkid);
        return -1;  // Trả về -1 nếu ID không hợp lệ
    }

    // Lấy thông tin của task bằng ID
    struct bktask_t *tsk = bktask_get_byid(bktaskid);
    
    // Kiểm tra tính hợp lệ của task
    if (tsk == NULL) {
        printf("Task ID %u not found\n", bktaskid);
        return -1;  
    }

    // Đánh dấu worker này là đang bận
    wrkid_busy[wrkid] = 1;

    // Gán task cho worker
    worker[wrkid].func = tsk->func;
    worker[wrkid].arg = tsk->arg;
    worker[wrkid].bktaskid = bktaskid;

    // In thông báo về việc gán task
    printf("Assigned task %d to worker %d\n", tsk->bktaskid, wrkid);
    
    return 0;  // Trả về 0 để biểu thị việc gán task thành công
}

int bkwrk_create_worker() {
    unsigned int i;

    for (i = 0; i < MAX_WORKER; i++) {
#ifdef WORK_THREAD
        void *child_stack = malloc(STACK_SIZE);
        if (child_stack == NULL) {
            fprintf(stderr, "Failed to allocate memory for child stack\n");
            continue;  // Skip this iteration if memory allocation fails
        }

        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGQUIT);
        sigaddset(&set, SIGUSR1);
        sigprocmask(SIG_BLOCK, &set, NULL);  // Block specified signals

        // Stack grows down, so start at the top
        void *stack_top = (void *)((char *)child_stack + STACK_SIZE - sizeof(void*));

        int *arg = malloc(sizeof(int));
        if (arg == NULL) {
            fprintf(stderr, "Failed to allocate memory for argument\n");
            free(child_stack);
            continue;
        }
        *arg = i;

        wrkid_tid[i] = clone(bkwrk_worker, stack_top, CLONE_VM | CLONE_FILES | SIGCHLD, (void *)arg);
        if (wrkid_tid[i] == -1) {
            fprintf(stderr, "Failed to create worker %u\n", i);
            free(arg);
            free(child_stack);
            continue;
        }

#ifdef INFO
        fprintf(stderr, "bkwrk_create_worker got worker %u\n", wrkid_tid[i]);
#endif
        usleep(100);
#else
        pid_t pid = fork();
        if (pid == 0) {  // Child process
            wrkid_tid[i] = getpid();
            int arg = i;
            bkwrk_worker((void *)&arg);
            exit(0);  // Exit after the work is done
        } else if (pid > 0) {  // Parent process
            wrkid_tid[i] = pid;
#ifdef INFO
            fprintf(stderr, "bkwrk_create_worker forked worker %u\n", pid);
#endif
        } else {
            fprintf(stderr, "Failed to fork worker process\n");
        }
#endif
    }

    return 0;  // All workers created
}

int bkwrk_get_worker() {
    static int last_assigned_worker = 0;  // Theo dõi worker cuối cùng được phân công

    for (int i = 0; i < MAX_WORKER; i++) {
        int idx = (last_assigned_worker + i) % MAX_WORKER;  // Bắt đầu từ worker tiếp theo sau worker cuối cùng được phân công
        if (wrkid_busy[idx] == 0) {
            wrkid_busy[idx] = 1;  // Đánh dấu worker là đang bận
            last_assigned_worker = idx;  // Cập nhật worker cuối cùng được phân công
            printf("Assigned worker %d\n", idx);  // Thêm thông tin debug
            return idx;  // Trả về ID của worker
        }
    }

    printf("No available worker\n");  // Thêm thông tin debug
    return -1;  // Trả về -1 nếu không có worker nào rảnh
}




int bkwrk_dispatch_worker(unsigned int wrkid) {
    if (wrkid >= MAX_WORKER) {
        fprintf(stderr, "Invalid worker ID: %u\n", wrkid);
        return -1;
    }

#ifdef WORK_THREAD
    unsigned int tid = wrkid_tid[wrkid]; // Lấy thread ID hoặc process ID

    /* Kiểm tra xem worker này có task hợp lệ để thực hiện không */
    if (worker[wrkid].func == NULL) {
        fprintf(stderr, "No function assigned to worker %u\n", wrkid);
        return -1;
    }

#ifdef DEBUG
    fprintf(stderr, "bkwrk dispatch wrkid %d - send signal %u \n", wrkid, tid);
#endif

    // Gửi tín hiệu để kích hoạt worker
    if (syscall(SYS_tkill, tid, SIG_DISPATCH) == -1) {
        fprintf(stderr, "Failed to send signal to thread %u\n", tid);
        return -1;
    }
#else
    // Trường hợp này dành cho việc sử dụng process thông thường
    pid_t pid = wrkid_tid[wrkid]; // Lấy PID của process

    /* Kiểm tra xem worker này có task hợp lệ để thực hiện không */
    if (worker[wrkid].func == NULL){
        fprintf(stderr, "No function assigned to worker %d\n", wrkid);
        return -1;
    }

#ifdef DEBUG
    fprintf(stderr, "bkwrk dispatch wrkid %d - send signal %u \n", wrkid, pid);
#endif

    // Gửi tín hiệu SIG_DISPATCH để kích hoạt process
    if (kill(pid, SIG_DISPATCH) == -1) {
        fprintf(stderr, "Failed to send signal to process %u\n", pid);
        return -1;
    }
#endif
    return 0; // Trả về 0 nếu việc dispatch thành công
}




