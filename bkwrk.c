#include "bktpool.h"
#include <signal.h>
#include <stdio.h>

#define _GNU_SOURCE
#include <linux/sched.h>
#include <sys/syscall.h>
#include <unistd.h>

//#define DEBUG
#define INFO
//#define WORK_THREAD

void *bkwrk_worker(void *arg) {
    sigset_t set;
    int sig;
    int s;
    int i = *((int *)arg);
    struct bkworker_t *wrk = &worker[i];

    /* Đặt mask để đợi tín hiệu */
    sigemptyset(&set); // Xóa tất cả các tín hiệu khỏi tập hợp
    sigaddset(&set, SIGUSR1); // Thêm SIGUSR1 vào tập hợp
    sigaddset(&set, SIGQUIT); // Thêm SIGQUIT vào tập hợp
    //sigaddset(&set, SIG_DISPATCH); // Thêm SIG_DISPATCH vào tập hợp

#ifdef DEBUG
    fprintf(stderr, "worker %i start living tid %d \n", i, getpid());
    fflush(stderr); // Đẩy dữ liệu từ buffer ra standard error
#endif

    while (1) {
        /* Chờ tín hiệu */
        s = sigwait(&set, &sig); // Chờ tín hiệu từ tập hợp và lưu tín hiệu nhận được vào biến sig
        if (s != 0) // Nếu không nhận được tín hiệu
            continue; // Tiếp tục vòng lặp

#ifdef INFO
        fprintf(stderr, "worker wake %d up\n", i);
#endif

        /* Kiểm tra xem tín hiệu có phải là SIG_DISPATCH không */
        //if (sig == SIG_DISPATCH) {
            /* Đang chạy công việc */
            if (wrk->func != NULL)
                wrk->func(wrk->arg); // Gọi hàm công việc với đối số được truyền vào

            /* Thông báo rằng đã hoàn thành công việc */
            wrkid_busy[i] = 0; // Đánh dấu worker không bận
            worker[i].func = NULL; // Đặt con trỏ hàm của worker thành NULL
            worker[i].arg = NULL; // Đặt con trỏ đối số của worker thành NULL
            worker[i].bktaskid = -1; // Đặt bktaskid của worker thành -1
        //}
    }
}

int bktask_assign_worker(unsigned int bktaskid, unsigned int wrkid) {

    // Kiểm tra xem ID của worker có hợp lệ hay không
    if (wrkid >= MAX_WORKER) {
        return -1;  // Trả về -1 nếu ID không hợp lệ
    }

    // Lấy thông tin của task bằng ID
    struct bktask_t *tsk = bktask_get_byid(bktaskid);
    if (tsk == NULL) {
        return -1;  // Trả về -1 nếu không tìm thấy task
    }

    // Đánh dấu worker này là đang bận
    wrkid_busy[wrkid] = 1;

    // Gán task cho worker
    worker[wrkid].func = tsk->func; // Lưu con trỏ hàm của task vào worker
    worker[wrkid].arg = tsk->arg; // Lưu con trỏ đối số của task vào worker
    worker[wrkid].bktaskid = bktaskid; // Lưu ID của task vào worker

    printf("Assigned task %d to worker %d\n", tsk->bktaskid, wrkid);

    return 0;  // Trả về 0 để biểu thị việc gán task thành công
}

int bkwrk_create_worker() {
    unsigned int i;

    for (i = 0; i < MAX_WORKER; i++) {
#ifdef WORK_THREAD
        void **child_stack = (void **)malloc(STACK_SIZE); // Cấp phát bộ nhớ cho stack của child thread
        if (child_stack == NULL) {
            fprintf(stderr, "Failed to allocate memory for child stack\n"); 
            continue;  // Bỏ qua lần lặp này nếu cấp phát bộ nhớ thất bại
        }

        sigset_t set;
        sigemptyset(&set); // Xóa tất cả các tín hiệu khỏi tập hợp
        sigaddset(&set, SIGQUIT); // Thêm SIGQUIT vào tập hợp
        sigaddset(&set, SIGUSR1); // Thêm SIGUSR1 vào tập hợp
        sigprocmask(SIG_BLOCK, &set, NULL);  // Chặn các tín hiệu đã chỉ định

        // Stack tăng dần, vì vậy bắt đầu từ đầu
        void *stack_top = (void *)((char *)child_stack + STACK_SIZE - sizeof(void*));

	// Tạo child thread
        wrkid_tid[i] = clone(&bkwrk_worker, stack_top, CLONE_VM | CLONE_FILES | SIGCHLD, (void *)&i); 
        
#ifdef INFO
        fprintf(stderr, "bkwrk_create_worker got worker %u\n", wrkid_tid[i]);
#endif
        usleep(100);
#else
        pid_t pid = fork(); // Tạo child process
        if (pid == 0) {  // Child process
            wrkid_tid[i] = getpid(); // Lấy PID của child process
            bkwrk_worker((void *)&i); // Gọi worker function trong child process
            exit(0);  // Thoát sau khi công việc được thực hiện
        } else if (pid > 0) {  // Parent process
            wrkid_tid[i] = pid; // Lưu PID của child process vào mảng
#ifdef INFO
            fprintf(stderr, "bkwrk_create_worker forked worker %u\n", pid); 
#endif
        } else {
            fprintf(stderr, "Failed to fork worker process\n"); 
        }
#endif
    }

    return 0;  // Tất cả các worker đã được tạo
}


int bkwrk_get_worker() {
    static int last_assigned_worker = 0;  // Theo dõi worker cuối cùng được phân công

    for (int i = 0; i < MAX_WORKER; i++) {
        int idx = (last_assigned_worker + i) % MAX_WORKER;  
        if (wrkid_busy[idx] == 0) {
            wrkid_busy[idx] = 1;  // Đánh dấu worker là đang bận
            last_assigned_worker = idx;  // Cập nhật worker cuối cùng được phân công
            return idx;  // Trả về ID của worker
        }
    }

    return -1;  // Trả về -1 nếu không có worker nào rảnh
}


int bkwrk_dispatch_worker(unsigned int wrkid) {
#ifdef WORK_THREAD
	unsigned int tid = wrkid_tid[wrkid]; // Lấy thread ID hoặc process ID

	/* Kiểm tra xem worker này có task hợp lệ để thực hiện không */
	if (worker[wrkid].func == NULL)
    	return -1; // Trả về -1 nếu không có công việc nào cho worker này

#ifdef DEBUG
	fprintf(stderr, "bkwrk dispatch wrkid %d - send signal %u \n", wrkid, tid);
#endif

	// Gửi tín hiệu để kích hoạt worker
	syscall(SYS_tkill, tid, SIG_DISPATCH); // Gửi tín hiệu SIG_DISPATCH đến worker thread hoặc process
#else
	// Trường hợp này dành cho việc sử dụng process thông thường
	pid_t pid = wrkid_tid[wrkid]; // Lấy PID của process

	/* Kiểm tra xem worker này có task hợp lệ để thực hiện không */
	if (worker[wrkid].func == NULL)
    	return -1; // Trả về -1 nếu không có công việc nào cho worker này

#ifdef DEBUG
	fprintf(stderr, "bkwrk dispatch wrkid %d - send signal %u \n", wrkid, pid);
#endif

	// Gửi tín hiệu SIG_DISPATCH để kích hoạt process
	if (kill(pid, SIG_DISPATCH) == -1) {
    	fprintf(stderr, "Failed to send signal to process %u\n", pid); // In ra thông báo nếu gửi tín hiệu thất bại
    	return -1; // Trả về -1 nếu gửi tín hiệu thất bại
	}
#endif
	return 0; // Trả về 0 nếu việc dispatch thành công
}
